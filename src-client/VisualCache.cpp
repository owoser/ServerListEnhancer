#include "VisualCache.h"

#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace sle::client {

namespace {

using nlohmann::json;

constexpr int kSchemaVersion = 1;

json toJson(VisualSpec const& spec) {
    json j;
    j["serverId"]      = spec.serverId;
    j["logoId"]        = spec.logoId;
    j["logoUrl"]       = spec.logoUrl;
    j["introText"]     = spec.introText;
    j["introImageId"]  = spec.introImageId;
    j["introImageUrl"] = spec.introImageUrl;
    return j;
}

VisualSpec fromJson(json const& j) {
    VisualSpec spec;
    auto read = [&j](char const* key, std::string& out) {
        if (auto it = j.find(key); it != j.end() && it->is_string()) {
            out = it->get<std::string>();
        }
    };
    read("serverId", spec.serverId);
    read("logoId", spec.logoId);
    read("logoUrl", spec.logoUrl);
    read("introText", spec.introText);
    read("introImageId", spec.introImageId);
    read("introImageUrl", spec.introImageUrl);
    return spec;
}

} // namespace

VisualCache& VisualCache::getInstance() {
    static VisualCache instance;
    return instance;
}

void VisualCache::setPath(std::filesystem::path path) {
    std::lock_guard<std::mutex> lock(mMutex);
    mPath = std::move(path);
}

bool VisualCache::load() {
    std::lock_guard<std::mutex> lock(mMutex);
    mEntries.clear();
    if (mPath.empty() || !std::filesystem::exists(mPath)) {
        return true; // 没有缓存是正常状态
    }

    std::ifstream in(mPath);
    if (!in) {
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    try {
        json const root = json::parse(buffer.str());
        if (root.value("schemaVersion", 0) != kSchemaVersion) {
            return false; // 结构不兼容：不猜测迁移，直接视为无缓存
        }
        if (auto it = root.find("entries"); it != root.end() && it->is_object()) {
            for (auto const& [key, value] : it->items()) {
                if (!value.is_object()) {
                    continue;
                }
                CachedVisual entry;
                entry.spec      = fromJson(value.value("spec", json::object()));
                entry.updatedAt = value.value("updatedAt", std::string{});
                mEntries.emplace(key, std::move(entry));
            }
        }
        return true;
    } catch (std::exception const&) {
        mEntries.clear(); // 损坏的缓存隔离，不影响列表
        return false;
    }
}

bool VisualCache::save() const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mPath.empty()) {
        return false;
    }

    json root;
    root["schemaVersion"] = kSchemaVersion;
    json entries          = json::object();
    for (auto const& [key, entry] : mEntries) {
        json item;
        item["spec"]      = toJson(entry.spec);
        item["updatedAt"] = entry.updatedAt;
        entries[key]      = std::move(item);
    }
    root["entries"] = std::move(entries);

    std::error_code ec;
    std::filesystem::create_directories(mPath.parent_path(), ec);

    // 原子替换：先写临时文件，再覆盖目标，避免半个 JSON
    auto tmp = mPath;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << root.dump(2);
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, mPath, ec);
    if (ec) {
        std::filesystem::remove(mPath, ec);
        std::filesystem::rename(tmp, mPath, ec);
    }
    return !ec;
}

void VisualCache::put(std::string const& endpointKey, CachedVisual value) {
    std::lock_guard<std::mutex> lock(mMutex);
    mEntries[endpointKey] = std::move(value);
}

std::optional<CachedVisual> VisualCache::get(std::string const& endpointKey) const {
    std::lock_guard<std::mutex> lock(mMutex);
    if (auto it = mEntries.find(endpointKey); it != mEntries.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::size_t VisualCache::size() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mEntries.size();
}

std::vector<std::pair<std::string, CachedVisual>> VisualCache::entries() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return {mEntries.begin(), mEntries.end()};
}

} // namespace sle::client
