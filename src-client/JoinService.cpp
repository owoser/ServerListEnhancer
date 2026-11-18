#include "JoinService.h"

#include "AddressParser.h"
#include "ClientEntry.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientCancelJoinLevelEvent.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientJoinLevelEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Symbol.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/world/JoinServerWorldResult.h"
#include "mc/client/world/NetworkWorldJoiner.h"
#include "mc/client/world/ServerWorldJoiner.h"
#include "mc/client/world/WorldSystem.h"
#include "mc/client/world/WorldSystemModules.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace sle::client {
namespace {

// Exact symbol from the LL 26.60.8 header/import library and mcapi request #352.
// A DLL-only upgrade can leave the game using an older bedrock_runtime_data.
constexpr ll::memory::SymbolView JoinSymbol{
    "?joinExternalServer@ServerWorldJoiner@World@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@G0V?$function@$$A6AXW4JoinServerWorldResult@World@@@Z@4@@Z"
};

// Only a session started by this module is ever moved back to the home screen.
enum class Stage { None, Joining, InWorld, AwaitingHome };

struct Run {
    std::atomic<bool>            enabled{true};
    std::atomic<bool>            queued{false};
    std::atomic<Stage>           stage{Stage::None};
    std::atomic<ClientInstance*> owner{nullptr};
    std::function<void()>        pending; // guarded by gMutex; consumed once after ClientInstance::update
    std::string                  lastScreen; // client update thread only
    std::chrono::steady_clock::time_point dispatchedAt{}; // set when a join is queued
    std::chrono::steady_clock::time_point lastBeat{}; // client update thread only
    std::chrono::steady_clock::time_point firstPop{}; // client update thread only
    int                          popCount{}; // client update thread only
};

std::mutex       gMutex;
std::shared_ptr<Run> gRun;
std::atomic<unsigned> gCallbacks{};
struct CallbackLease {
    CallbackLease() { ++gCallbacks; }
    ~CallbackLease() { --gCallbacks; }
};
ll::event::ListenerPtr gJoined, gExited, gCanceled;

auto& log() { return ClientEntry::getInstance().getSelf().getLogger(); }

int stageValue(Stage stage) { return static_cast<int>(stage); }

// Vanilla publishes the exit event before the level is torn down, so the pop is
// scheduled from the next ClientInstance::update once the menu screen settles.
void pumpJoinService(ClientInstance& client) {
    std::shared_ptr<Run>  run;
    std::function<void()> pending;
    {
        std::lock_guard lock(gMutex);
        run = gRun;
        if (!run || !run->enabled.load()) return;
        pending = std::exchange(run->pending, {});
    }
    if (pending) pending();
    if (!run->enabled.load() || run->stage.load() != Stage::AwaitingHome) return;

    auto const stack = client.getCurrentSceneStack();
    auto const name  = stack->getScreenName();
    auto const now = std::chrono::steady_clock::now();
    if (name != run->lastScreen || now - run->lastBeat > std::chrono::seconds(1)) {
        run->lastBeat   = now;
        run->lastScreen = name;
        std::string screens;
        for (auto const& screen : stack->getScreenNames()) screens += screen + ";";
        log().info(
            "[join] awaiting home: screen={} level={} player={} scheduled={}/{} stack={}",
            name,
            bool(client.getLevel()),
            bool(client.getLocalPlayer()),
            stack->hasScheduledScreens(),
            stack->hasScheduledEvents(),
            screens
        );
        log().flush();
    }
    if (client.getLevel() || client.getLocalPlayer()) return;
    if (stack->hasScheduledScreens() || stack->hasScheduledEvents()) return;
    // The world list is an OreUI view in 1.26.60, not the legacy play_screen.
    bool const isWorldList = name == "play_screen" || name == "ui://./hbui/index.html";
    if (name == "start_screen") {
        run->stage.store(Stage::None);
        run->owner.store(nullptr);
        log().info("[join] owned session returned to the home screen");
        log().flush();
        return;
    }
    if (!isWorldList) return;
    auto const names = stack->getScreenNames();
    if (std::find(names.begin(), names.end(), "start_screen") == names.end()) return;
    if (run->popCount == 0) {
        run->firstPop = now;
        ++run->popCount;
        stack->schedulePopScreenWithExpectedNames({name});
        log().info("[join] scheduled a named pop from play_screen back to start_screen");
        log().flush();
        return;
    }
    // The named pop does not always match the OreUI play screen; fall back to a
    // single plain pop once the named attempt had a frame to take effect.
    if (run->popCount == 1 && now - run->firstPop > std::chrono::milliseconds(1200)) {
        ++run->popCount;
        stack->schedulePopScreen(1);
        log().info("[join] named pop had no effect; scheduled a single pop instead");
        log().flush();
    }
}

LL_TYPE_INSTANCE_HOOK(
    JoinUpdateHook, ll::memory::HookPriority::Normal, ClientInstance,
    &ClientInstance::$update, bool, bool isInitFinished, bool allowGameUpdate
) {
    auto const result = origin(isInitFinished, allowGameUpdate);
    // Menu and transition frames may report isInitFinished=false, but the owned
    // session still has to be walked back to the home screen there.
    pumpJoinService(*this);
    return result;
}
std::unique_ptr<ll::memory::HookRegistrar<JoinUpdateHook>> gUpdateHook;

} // namespace

void startJoinService() {
    auto run = std::make_shared<Run>();
    { std::lock_guard lock(gMutex); gRun = run; }

    auto& bus  = ll::event::EventBus::getInstance();
    gJoined = bus.emplaceListener<ll::event::ClientJoinLevelEvent>([run](auto& event) {
        auto const stage = run->stage.load();
        log().info(
            "[join] join-level event stage={} owner={} client={}",
            stageValue(stage),
            static_cast<void*>(run->owner.load()),
            static_cast<void*>(&event.self())
        );
        log().flush();
        if (!run->enabled.load()) return;
        // Bedrock also fires cancel-join while a successful join is switching
        // screens, so entering the world only has to happen inside the window.
        if (run->stage.load() != Stage::Joining) return;
        if (std::chrono::steady_clock::now() - run->dispatchedAt > std::chrono::seconds(120)) return;
        run->stage.store(Stage::InWorld);
        log().info("[join] owned session entered the world");
        log().flush();
    });
    gExited = bus.emplaceListener<ll::event::ClientExitLevelEvent>([run](auto& event) {
        auto const stage = run->stage.load();
        log().info(
            "[join] exit-level event stage={} owner={} client={}",
            stageValue(stage),
            static_cast<void*>(run->owner.load()),
            static_cast<void*>(&event.self())
        );
        log().flush();
        if (!run->enabled.load()) return;
        auto expected = Stage::InWorld;
        if (run->stage.compare_exchange_strong(expected, Stage::AwaitingHome)) {
            run->lastScreen.clear();
            run->popCount = 0;
            log().info("[join] owned session exited; waiting for the menu screen");
            log().flush();
        }
    });
    gCanceled = bus.emplaceListener<ll::event::ClientCancelJoinLevelEvent>([run](auto& event) {
        auto const stage = run->stage.load();
        log().info("[join] cancel-join event stage={} client={}", stageValue(stage), static_cast<void*>(&event.self()));
        log().flush();
        // Diagnostic only: this also fires during a successful join.
        (void)event;
    });
    gUpdateHook = std::make_unique<ll::memory::HookRegistrar<JoinUpdateHook>>();
}

void stopJoinService() {
    {
        std::lock_guard lock(gMutex);
        if (gRun) {
            gRun->enabled.store(false);
            gRun->pending = {};
        }
        gRun.reset();
    }
    gUpdateHook.reset();
    auto& bus = ll::event::EventBus::getInstance();
    for (auto* listener : {&gJoined, &gExited, &gCanceled}) {
        if (*listener) bus.removeListener(*listener);
        listener->reset();
    }
}

bool canUnloadJoinService() { return gCallbacks.load() == 0; }

bool joinServer(std::string address, unsigned short port, std::string name, JoinFeedback feedback) {
    auto reject = [&](std::string message) {
        if (feedback) feedback(false, message);
        return false;
    };
    if (!validServerEndpoint(address, port)) return reject("地址无效，请先编辑服务器");
    // Do this before ever dereferencing game objects or calling the imported stub.
    if (!JoinSymbol.resolve(true)) {
        log().warn("[join] joinExternalServer is unavailable; update LL and bedrock_runtime_data together");
        return reject("连接接口不可用，请完整升级 LeviLamina 26.60.8 后重启游戏");
    }

    std::shared_ptr<Run> run;
    { std::lock_guard lock(gMutex); run = gRun; }
    if (!run || !run->enabled.load()) return reject("连接服务尚未就绪");
    if (run->queued.exchange(true)) return reject("正在发起连接，请稍候");

    auto lease = std::make_shared<CallbackLease>();
    std::lock_guard lock(gMutex);
    if (gRun != run || !run->enabled.load()) {
        run->queued.store(false);
        return reject("连接服务已停用");
    }
    run->pending = [run, lease, address = std::move(address), port, name = std::move(name),
                    feedback = std::move(feedback)] {
        if (!run->enabled.load()) return;
        auto fail = [&](std::string const& message) {
            run->queued.store(false);
            if (feedback) feedback(false, message);
        };
        auto client = ll::service::bedrock::getClientInstance();
        if (!client) {
            fail("客户端尚未就绪，请稍后重试");
            return;
        }
        auto& modules = client->getWorldSystem().mModules.get();
        auto* joiner  = modules.mNetworkWorldJoiner.get();
        if (!joiner) {
            fail("服务器连接器尚未就绪，请稍后重试");
            return;
        }
        run->owner.store(&*client);
        run->dispatchedAt = std::chrono::steady_clock::now();
        run->stage.store(Stage::Joining);
        log().info("[join] dispatching {}:{} ({}) after ClientInstance::update", address, port, name);
        log().flush();
        // Close/invalidate the overlay before the game changes screens.
        if (feedback) feedback(true, {});
        joiner->mServerWorldJoiner.get().joinExternalServer(
            address,
            port,
            name,
            [run, lease, feedback](World::JoinServerWorldResult result) {
                if (!run->enabled.load()) return;
                run->stage.store(Stage::None);
                run->owner.store(nullptr);
                log().warn("[join] game reported connection error={}", static_cast<int>(result));
                log().flush();
                if (feedback) feedback(false, "加入服务器失败（错误码 " + std::to_string(static_cast<int>(result)) + "）");
            }
        );
        run->queued.store(false);
    };
    return true;
}

} // namespace sle::client
