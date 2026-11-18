// 统一内存分配运算符：LeviLamina 要求模组使用 LL 的分配器，否则拒绝加载
#define LL_MEMORY_OPERATORS
#include "ll/api/memory/MemoryOperators.h"

#include "ClientEntry.h"

#include "ProbeOverlay.h"
#include "ImageProvider.h"
#include "LatencyProbe.h"
#include "JoinService.h"
#include "LoaderVersion.h"
#include "ServerStatus.h"
#include "common/Endpoint.h"
#include "ServerStore.h"
#include "SessionTracker.h"
#include "VisualCache.h"
#include "VisualPacketHandler.h"

#include "ll/api/Versions.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientStartJoinLevelEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"

#include <filesystem>

namespace sle::client {

namespace {
// 全局实例存在即触发 PacketRegistrar 注册
[[maybe_unused]] VisualPacketHandler gPacketHandler;
} // namespace

ClientEntry& ClientEntry::getInstance() {
    static ClientEntry instance;
    return instance;
}

ClientEntry::ClientEntry() : mSelf(*ll::mod::NativeMod::current()) {}

bool ClientEntry::load() {
    auto& logger = getSelf().getLogger();
    // LL 不在普通模组依赖集合中；在任何客户端初始化前检查实际运行版本。
    auto const loaderVersion = ll::getLoaderVersion();
    if (!isSupportedLoaderVersion(loaderVersion)) {
        logger.error(
            "ServerListEnhancer (client) 需要 LeviLamina >={}，当前版本 {}，拒绝加载",
            kMinimumLoaderVersion,
            loaderVersion.to_string()
        );
        return false;
    }
    logger.info("ServerListEnhancer (client) 加载中...");

    // mods/ServerListEnhancer/config -> mods/ServerListEnhancer/cache/visuals.json
    auto cachePath = getSelf().getConfigDir().parent_path() / "cache" / "visuals.json";
    cachePath.make_preferred();

    auto& cache = VisualCache::getInstance();
    cache.setPath(cachePath);
    if (cache.load()) {
        logger.info("视觉缓存已加载: {} ({} 条)", cachePath.string(), cache.size());
    } else {
        logger.warn("视觉缓存读取失败或结构不兼容，按空缓存启动: {}", cachePath.string());
    }

    // mods/ServerListEnhancer/servers.json：自维护的服务器列表
    auto serverPath = getSelf().getConfigDir().parent_path() / "servers.json";
    serverPath.make_preferred();
    auto& store = ServerStore::getInstance();
    store.setPath(serverPath);
    if (store.load()) {
        logger.info("服务器列表已加载: {} ({} 条)", serverPath.string(), store.list().size());
    } else {
        logger.warn("服务器列表读取失败或结构不兼容，按空列表启动: {}", serverPath.string());
    }

    ImageProvider::getInstance().initialize(getSelf().getConfigDir().parent_path() / "cache" / "images");
    ServerStatus::getInstance().setCachePath(getSelf().getConfigDir().parent_path() / "cache" / "discovery.json");
    logger.info("内置图片就绪，客户端无需 DearOreUI 前置");
    logger.info("ServerListEnhancer (client) 已加载");
    return true;
}

bool ClientEntry::enable() {
    startJoinService();
    ImageProvider::getInstance().start();
    LatencyProbe::getInstance().start();
    // Legacy NetherNet status metadata only; image downloads always validate TLS.
    ServerStatus::getInstance().start({.allowSelfSignedNetherNet=true, .allowPlainHttpFallback=true});
    for (auto const& s : ServerStore::getInstance().list())
        ServerStatus::getInstance().request(endpointKey(s.address,s.port),s.address,s.port);
    if (!mJoinListener) {
        mJoinListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::client::ClientStartJoinLevelEvent>(
            [](ll::event::client::ClientStartJoinLevelEvent& event) {
                SessionTracker::getInstance().onStartJoin(event.serverName(), event.worldName());
                ClientEntry::getInstance().getSelf().getLogger().info(
                    "开始连接服务器: name={}, world={}",
                    event.serverName(),
                    event.worldName()
                );
            }
        );
    }
    if (!mExitListener) {
        mExitListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::client::ClientExitLevelEvent>(
            [](ll::event::client::ClientExitLevelEvent&) {
                SessionTracker::getInstance().onExit();
                ClientEntry::getInstance().getSelf().getLogger().info("已退出服务器，当前会话失效");
            }
        );
    }

    // Use a single native JSON-UI rendering path. The experimental HTML
    // injection is intentionally not started (it targeted an invisible view).

    // P1 探针：验证 AfterUIRenderEvent 叠加可见性与鼠标坐标命中
    registerProbeOverlay();

    getSelf().getLogger().info("ServerListEnhancer (client) 已启用");
    return true;
}

bool ClientEntry::disable() {
    stopJoinService();
    if (mJoinListener) {
        ll::event::EventBus::getInstance().removeListener(mJoinListener);
        mJoinListener.reset();
    }
    if (mExitListener) {
        ll::event::EventBus::getInstance().removeListener(mExitListener);
        mExitListener.reset();
    }
    unregisterProbeOverlay();
    ServerStatus::getInstance().stop();
    ImageProvider::getInstance().stop();
    LatencyProbe::getInstance().stop();
    VisualCache::getInstance().save();
    return true;
}

bool ClientEntry::unload() {
    if (!canUnloadJoinService()) {
        getSelf().getLogger().warn("[join] A game callback is still active; restart the client to unload safely");
        return false;
    }
    getSelf().getLogger().info("ServerListEnhancer (client) 已卸载");
    return true;
}

} // namespace sle::client

LL_REGISTER_MOD(sle::client::ClientEntry, sle::client::ClientEntry::getInstance());
