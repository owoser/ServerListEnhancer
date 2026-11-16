#include "ServerConfig.h"

#include "common/Validation.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace sle::server {

namespace {

using nlohmann::json;

json toJson(ServerConfig const& cfg) {
    json j;
    j["version"]       = cfg.version;
    j["enabled"]       = cfg.enabled;
    j["logLevel"]      = cfg.logLevel;
    j["serverId"]      = cfg.visual.serverId;
    j["logoId"]        = cfg.visual.logoId;
    j["logoUrl"]       = cfg.visual.logoUrl;
    j["introText"]     = cfg.visual.introText;
    j["introImageId"]  = cfg.visual.introImageId;
    j["introImageUrl"] = cfg.visual.introImageUrl;
    return j;
}

// 逐字段读取：缺失键保持默认值，类型不符直接忽略该字段
void applyJson(json const& j, ServerConfig& cfg) {
    auto readString = [&j](char const* key, std::string& out) {
        if (auto it = j.find(key); it != j.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    if (auto it = j.find("version"); it != j.end() && it->is_number_integer()) {
        cfg.version = it->get<int>();
    }
    if (auto it = j.find("enabled"); it != j.end() && it->is_boolean()) {
        cfg.enabled = it->get<bool>();
    }
    if (auto it = j.find("logLevel"); it != j.end() && it->is_number_integer()) {
        cfg.logLevel = it->get<int>();
    }
    readString("serverId", cfg.visual.serverId);
    readString("logoId", cfg.visual.logoId);
    readString("logoUrl", cfg.visual.logoUrl);
    readString("introText", cfg.visual.introText);
    readString("introImageId", cfg.visual.introImageId);
    readString("introImageUrl", cfg.visual.introImageUrl);

    if (cfg.visual.serverId.empty()) {
        cfg.visual.serverId = "my-server";
    }
    // 清洗：非法 id / URL 清空后回退到内置资源或纯文本
    validation::sanitizeId(cfg.visual.serverId);
    validation::sanitizeId(cfg.visual.logoId);
    validation::sanitizeId(cfg.visual.introImageId);
    validation::sanitizeUrl(cfg.visual.logoUrl);
    validation::sanitizeUrl(cfg.visual.introImageUrl);
    validation::sanitizeText(cfg.visual.introText, kMaxIntroTextLength);

    if (cfg.visual.serverId.empty()) {
        cfg.visual.serverId = "my-server";
    }
}

} // namespace

ConfigManager& ConfigManager::getInstance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::load(std::filesystem::path const& path) {
    mPath = path;
    mPath.make_preferred();

    if (!std::filesystem::exists(mPath)) {
        ServerConfig defaults{};
        defaults.visual.serverId     = "my-server";
        defaults.visual.logoId       = "default";
        defaults.visual.introImageId = "default";
        defaults.visual.introText    = "欢迎来到本服务器";
        std::ofstream out(mPath);
        if (out) {
            out << toJson(defaults).dump(2) << std::endl;
        }
        mConfig = defaults;
        return true;
    }

    std::ifstream in(mPath);
    if (!in) {
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    // allow_exceptions=false：解析失败返回 discarded value 而不是抛异常
    auto parsed = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }

    ServerConfig next = mConfig;
    applyJson(parsed, next);
    mConfig = next;
    return true;
}

bool ConfigManager::reload() {
    if (mPath.empty()) {
        return false;
    }
    return load(mPath);
}

} // namespace sle::server
