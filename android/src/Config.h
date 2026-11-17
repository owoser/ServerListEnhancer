#pragma once
#include <pl/Config.hpp>
namespace sle::android {
struct Config {
    int version{1};
    bool enabled{true};
    bool autoRefresh{true};
    bool allowPlainHttpFallback{true};
    int udpProbePort{0};
};
}
template<> struct pl::config::Schema<sle::android::Config> {
    static constexpr std::string_view title = "服务器列表增强 / Android";
    static constexpr std::string_view description = "通过 Mod Menu 的服务器按钮打开独立列表";
    static constexpr FieldSchema field(std::string_view key) {
        if(key == "version") return {.title="配置版本", .readOnly=true};
        if(key == "enabled") return {.title="启用模组"};
        if(key == "autoRefresh") return {.title="启动时自动查询服务器"};
        if(key == "allowPlainHttpFallback") return {.title="兼容 NetherNet 明文 HTTP", .description="仅用于公开服务器资料；图片 HTTPS 证书始终严格校验"};
        if(key == "udpProbePort") return {.title="UDP 探测端口", .description="0 表示使用服务器游戏端口"};
        return {};
    }
};
