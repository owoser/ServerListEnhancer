#pragma once

#include "common/VisualSpec.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sle::client {

// 单台服务器的外观快照。主键是客户端决定的端点键，不是服务端自报的 serverId。
struct CachedVisual {
    VisualSpec  spec;
    std::string updatedAt; // epoch 秒，排查用
};

// 端点键 -> 外观快照，持久化到 mods/ServerListEnhancer/cache/visuals.json
class VisualCache {
public:
    static VisualCache& getInstance();

    void setPath(std::filesystem::path path);
    bool load();
    bool save() const;

    void                                       put(std::string const& endpointKey, CachedVisual value);
    [[nodiscard]] std::optional<CachedVisual>  get(std::string const& endpointKey) const;
    [[nodiscard]] std::size_t                  size() const;
    // 快照列举：复制一份，供 UI 渲染线程使用（避免持锁回调）
    [[nodiscard]] std::vector<std::pair<std::string, CachedVisual>> entries() const;
    [[nodiscard]] std::filesystem::path const& path() const { return mPath; }

private:
    mutable std::mutex                  mMutex;
    std::map<std::string, CachedVisual> mEntries;
    std::filesystem::path               mPath;
};

} // namespace sle::client