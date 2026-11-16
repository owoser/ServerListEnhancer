#include "SessionTracker.h"

#include <utility>

namespace sle::client {

namespace {
// 名称作为缓存键前做最小规范化：去首尾空白、转小写，避免大小写/空格造成重复条目
std::string normalizeName(std::string name) {
    auto const notSpace = [](unsigned char c) { return c != ' ' && c != '\t'; };
    while (!name.empty() && !notSpace(static_cast<unsigned char>(name.front()))) {
        name.erase(name.begin());
    }
    while (!name.empty() && !notSpace(static_cast<unsigned char>(name.back()))) {
        name.pop_back();
    }
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return name;
}
} // namespace

SessionTracker& SessionTracker::getInstance() {
    static SessionTracker instance;
    return instance;
}

void SessionTracker::onStartJoin(std::string serverName, std::string worldName) {
    std::lock_guard<std::mutex> lock(mMutex);
    mState            = SessionState{};
    mState.generation = ++mNextGeneration;
    mState.serverName = std::move(serverName);
    mState.worldName  = std::move(worldName);
    mState.active     = true;

    // 客户端拿不到原始 host:port（getAddress 在客户端为空），改用原版列表条目名归属。
    // 条目名来自玩家在列表里点击的那一项，而不是服务端自报的 serverId。
    auto const key = normalizeName(mState.serverName);
    if (!key.empty()) {
        mState.endpointKey = "name:" + key;
    }
}

void SessionTracker::onExit() {
    std::lock_guard<std::mutex> lock(mMutex);
    mState.active = false;
    mState.netId  = 0;
    mState.remoteAddress.clear();
    mState.remoteString.clear();
    mState.endpointKey.clear();
}

SessionState SessionTracker::snapshot() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return mState;
}

std::optional<std::string> SessionTracker::bindPacketSource(
    std::uint64_t netId,
    std::string   remoteAddress,
    std::string   remoteString
) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mState.active) {
        return std::nullopt;
    }
    if (mState.netId == 0) {
        mState.netId         = netId;
        mState.remoteAddress = std::move(remoteAddress);
        mState.remoteString  = std::move(remoteString);
    } else if (mState.netId != netId) {
        // 连接已变化（重连/切服），不接受旧会话之外的包
        return std::nullopt;
    }
    if (mState.endpointKey.empty()) {
        return std::nullopt; // 没有可归属的原版条目名
    }
    return mState.endpointKey;
}

} // namespace sle::client
