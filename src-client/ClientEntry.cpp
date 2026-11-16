// 统一内存分配运算符：LeviLamina 要求模组使用 LL 的分配器，否则拒绝加载
#define LL_MEMORY_OPERATORS
#include "ll/api/memory/MemoryOperators.h"

#include "ClientEntry.h"

#include "ProbeOverlay.h"
#include "ServerStore.h"
#include "SessionTracker.h"
#include "VisualCache.h"
#include "VisualPacketHandler.h"

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

    logger.info("ServerListEnhancer (client) 已加载");
    return true;
}

bool ClientEntry::enable() {
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

    // P1 探针：验证 AfterUIRenderEvent 叠加可见性与鼠标坐标命中
    registerProbeOverlay();

    getSelf().getLogger().info("ServerListEnhancer (client) 已启用");
    return true;
}

bool ClientEntry::disable() {
    if (mJoinListener) {
        ll::event::EventBus::getInstance().removeListener(mJoinListener);
        mJoinListener.reset();
    }
    if (mExitListener) {
        ll::event::EventBus::getInstance().removeListener(mExitListener);
        mExitListener.reset();
    }
    unregisterProbeOverlay();
    VisualCache::getInstance().save();
    return true;
}

bool ClientEntry::unload() {
    getSelf().getLogger().info("ServerListEnhancer (client) 已卸载");
    return true;
}

} // namespace sle::client

LL_REGISTER_MOD(sle::client::ClientEntry, sle::client::ClientEntry::getInstance());
