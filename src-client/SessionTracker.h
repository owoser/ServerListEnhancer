#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace sle::client {

// 当前这次连接的会话状态。generation 每次进入世界自增，用于丢弃迟到的包与异步回调。
struct SessionState {
    std::uint64_t generation    = 0;
    std::uint64_t netId         = 0;    // 首个自定义包到达时的连接哈希
    std::string   serverName;           // 原版列表里的条目名（ClientStartJoinLevelEvent 提供）
    std::string   worldName;
    std::string   remoteAddress;        // NetworkIdentifier::getAddress()（客户端可能为空）
    std::string   remoteString;         // NetworkIdentifier::toString()（排查用）
    std::string   endpointKey;          // 缓存键：name:<条目名>
    bool          active = false;
};

class SessionTracker {
public:
    static SessionTracker& getInstance();

    void onStartJoin(std::string serverName, std::string worldName);
    void onExit();

    [[nodiscard]] SessionState snapshot() const;

    // 收到自定义包：记录真实连接并返回缓存键；没有活动会话或无法归属时返回 nullopt
    std::optional<std::string> bindPacketSource(
        std::uint64_t netId,
        std::string   remoteAddress,
        std::string   remoteString
    );

private:
    mutable std::mutex mMutex;
    SessionState       mState{};
    std::uint64_t      mNextGeneration = 0;
};

} // namespace sle::client
