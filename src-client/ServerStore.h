#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sle::client {

// 本地维护的服务器条目（完全独立于原版 ExternalServerWorldList）
struct SavedServer {
    std::string  id;
    std::string  name;
    std::string  address;
    int          port{19132};
    std::string  logoUrl;
    std::string  logoId;
    std::string  introText;
    std::string  introImageUrl;
    std::int64_t lastJoinedAt{0};

    // 状态查询（NetherNet /v1/join）结果缓存
    std::string  statusName;
    int          players{-1};
    int          maxPlayers{-1};
    std::string  version;
    int          protocol{0};
    std::int64_t statusAt{0};
};

// mods/ServerListEnhancer/servers.json 的读写，线程安全，原子落盘
class ServerStore {
public:
    static ServerStore& getInstance();

    void setPath(std::filesystem::path path);
    bool load();
    bool save() const;

    [[nodiscard]] std::vector<SavedServer>      list() const;
    [[nodiscard]] std::optional<SavedServer>    get(std::string const& id) const;
    std::string                                 upsert(SavedServer server); // 返回 id
    bool                                        remove(std::string const& id);
    [[nodiscard]] std::filesystem::path const&  path() const { return mPath; }

private:
    mutable std::mutex       mMutex;
    std::vector<SavedServer> mServers;
    std::filesystem::path    mPath;
};

} // namespace sle::client
