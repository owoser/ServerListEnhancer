#pragma once

#include "ServerConfig.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace sle::server::join_info {

inline constexpr int kSchemaVersion = 1;
inline constexpr int kCacheTtlSeconds = 300;

inline nlohmann::json imageReference(std::string const& url, std::string const& id) {
    if (!url.empty()) {
        return {{"type", "url"}, {"value", url}};
    }
    return {{"type", "builtin"}, {"value", id.empty() ? "default" : id}};
}

inline std::string revisionOf(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buffer[17]{};
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

inline nlohmann::json buildPayload(ServerConfig const& config, bool runtimeEnabled) {
    bool const enabled = runtimeEnabled && config.enabled;
    nlohmann::json payload{
        {"schemaVersion", kSchemaVersion},
        {"cacheTtlSeconds", kCacheTtlSeconds},
        {"enabled", enabled},
        {"serverId", config.visual.serverId},
        {"logo", enabled ? imageReference(config.visual.logoUrl, config.visual.logoId) : imageReference({}, {})},
        {"introText", enabled ? config.visual.introText : std::string{}},
        {"introImage", enabled ? imageReference(config.visual.introImageUrl, config.visual.introImageId) : imageReference({}, {})},
    };
    if (enabled && !config.logoData.empty()) {
        payload["logo"] = {{"type", "data"}, {"value", config.logoData}};
    }
    // This is a change token, not an authentication or ownership proof.
    payload["revision"] = revisionOf(payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    return payload;
}

class SnapshotStore {
public:
    using Snapshot = std::shared_ptr<nlohmann::json const>;

    void publish(nlohmann::json payload) {
        mSnapshot.store(std::make_shared<nlohmann::json const>(std::move(payload)), std::memory_order_release);
    }

    [[nodiscard]] Snapshot load() const { return mSnapshot.load(std::memory_order_acquire); }

    [[nodiscard]] std::string extend(std::string const& original) const {
        auto const snapshot = load();
        if (!snapshot) {
            return original;
        }
        auto body = nlohmann::json::parse(original, nullptr, false);
        if (!body.is_object()) {
            return original;
        }
        body["server_list_enhancer"] = *snapshot;
        return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

private:
    std::atomic<Snapshot> mSnapshot{};
};

} // namespace sle::server::join_info
