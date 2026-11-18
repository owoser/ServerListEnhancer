// 统一内存分配运算符：LeviLamina 要求模组使用 LL 的分配器，否则拒绝加载
#define LL_MEMORY_OPERATORS
#include "ll/api/memory/MemoryOperators.h"

#include "ServerEntry.h"

#include "ServerConfig.h"
#include "JoinInfoExtension.h"
#include "UdpEcho.h"

#include "ll/api/command/Command.h"
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/io/Logger.h"
#include "ll/api/mod/RegisterHelper.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"

#include <filesystem>
#include <string>

namespace sle::server {

// Reflection of required parameter names needs externally linked types (Boost.PFR).
enum class AdminAction { reload };
struct ActionParam {
    AdminAction action;
};

namespace {
constexpr char kCommandName[] = "sle";

void registerCommand() {
    auto& registrar = ll::command::CommandRegistrar::getInstance(false);
    registrar.tryRegisterEnum<AdminAction>();

    auto& command = registrar.getOrCreateCommand(kCommandName, "服务器列表增强插件管理命令", CommandPermissionLevel::GameDirectors);

    command.overload<ActionParam>().required("action").execute(
        [](CommandOrigin const&, CommandOutput& output, ActionParam const& param, Command const&) {
            switch (param.action) {
            case AdminAction::reload:
                if (ConfigManager::getInstance().reload()) {
                    startUdpEcho(ConfigManager::getInstance().get().udpProbePort);
                    refreshJoinInfoSnapshot();
                    output.success("配置已重新加载，/v1/join 快照已发布");
                } else {
                    output.error("配置重新加载失败，请检查 config.json");
                }
                break;
            }
        }
    );
}

} // namespace

ServerEntry& ServerEntry::getInstance() {
    static ServerEntry instance;
    return instance;
}

ServerEntry::ServerEntry() : mSelf(*ll::mod::NativeMod::current()) {}

bool ServerEntry::load() {
    auto& logger = getSelf().getLogger();
    logger.info("ServerListEnhancer (server) 加载中...");

    auto configDir = getSelf().getConfigDir();
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) {
        logger.error("创建配置目录失败: {}", ec.message());
        return false;
    }
    auto configPath = configDir / "config.json";
    configPath.make_preferred();

    if (!ConfigManager::getInstance().load(configPath)) {
        logger.error("读取配置失败: {}", configPath.string());
        return false;
    }
    auto const& cfg = ConfigManager::getInstance().get();
    logger.setLevel(static_cast<ll::io::LogLevel>(cfg.logLevel));
    logger.info("配置已加载: {}", configPath.string());
    refreshJoinInfoSnapshot();
    return true;
}

bool ServerEntry::enable() {
    if (!installJoinInfoExtension()) return false;
    if(!startUdpEcho(ConfigManager::getInstance().get().udpProbePort))getSelf().getLogger().warn("UDP探测端口未启用，请检查配置/端口占用");
    setJoinInfoEnabled(true);
    registerCommand();
    getSelf().getLogger().info("ServerListEnhancer (server) 已启用");
    return true;
}

bool ServerEntry::disable() {
    stopUdpEcho();
    uninstallJoinInfoExtension();
    return true;
}

bool ServerEntry::unload() {
    getSelf().getLogger().warn("HTTP 回调由游戏持有；不支持热卸载，请重启服务器更新模组。");
    return false;
}

} // namespace sle::server

LL_REGISTER_MOD(sle::server::ServerEntry, sle::server::ServerEntry::getInstance());
