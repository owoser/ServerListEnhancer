#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace sle::client {

// NetherNet /v1/join 查询结果
struct ServerStatusResult {
    bool         ok{false};
    std::string  name;
    std::string  version;
    std::string  level;
    int          protocol{0};
    int          players{-1};
    int          maxPlayers{-1};
    std::string  error;
    std::int64_t at{0};
};

// 后台查询服务器状态，UI 线程只读快照
class ServerStatus {
public:
    static ServerStatus& getInstance();

    // 发起异步查询（key 一般是 address:port，60 秒内不重复请求）
    void request(std::string const& key, std::string const& address, int port);

    [[nodiscard]] std::optional<ServerStatusResult> get(std::string const& key) const;

private:
    mutable std::mutex                                  mMutex;
    std::unordered_map<std::string, ServerStatusResult> mResults;
    std::unordered_map<std::string, std::int64_t>       mInFlight;
    int                                                 mConcurrent{0};
};

} // namespace sle::client
