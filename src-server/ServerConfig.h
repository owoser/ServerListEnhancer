#pragma once

#include "common/VisualSpec.h"

#include <filesystem>
#include <string>

namespace sle::server {

// plugins/ServerListEnhancer/config.json
struct ServerConfig {
    int         version  = 1;      // 配置结构版本，便于后续迁移
    bool        enabled  = true;   // 总开关：false 时不下发任何视觉档案
    VisualSpec  visual;            // 下发给客户端的档案
    std::string logoData;          // HTTP-only PNG data URI; never changes the binary VisualSpec protocol
    int         udpProbePort = 0; // opt-in separate UDP echo, 0 disabled
    int         logLevel = 4;      // 0 Trace / 1 Debug / 2 Info / 3 Warn / 4 Error
};

class ConfigManager {
public:
    static ConfigManager& getInstance();

    // 文件不存在时写入默认配置；解析失败返回 false 并保留上一次成功的配置
    bool load(std::filesystem::path const& path);
    bool reload();

    [[nodiscard]] ServerConfig const& get() const { return mConfig; }
    [[nodiscard]] std::filesystem::path const& getPath() const { return mPath; }

private:
    ServerConfig        mConfig{};
    std::filesystem::path mPath{};
};

} // namespace sle::server
