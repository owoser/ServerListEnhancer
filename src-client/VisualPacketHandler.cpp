#include "VisualPacketHandler.h"

namespace sle::client {

void VisualPacketHandler::handlePacket(
    NetworkIdentifier const&   netId,
    NetEventCallback&,
    ll::network::PacketSide,
    ServerVisualPacket const& packet
) const {
    auto& logger = ClientEntry::getInstance().getSelf().getLogger();

    auto const hash = static_cast<unsigned long long>(netId.getHash());
    auto key        = SessionTracker::getInstance().bindPacketSource(
        static_cast<std::uint64_t>(hash),
        netId.getAddress(),
        netId.toString()
    );

    if (!key) {
        auto const state = SessionTracker::getInstance().snapshot();
        logger.warn(
            "收到视觉档案但无法归属到列表条目，已丢弃 (netId={:#x}, 会话名='{}', active={})",
            hash,
            state.serverName,
            state.active
        );
        return;
    }

    CachedVisual entry;
    entry.spec      = packet.spec;
    entry.updatedAt = std::to_string(static_cast<long long>(std::time(nullptr)));

    auto& cache = VisualCache::getInstance();
    cache.put(*key, std::move(entry));
    bool const saved = cache.save();

    auto const state = SessionTracker::getInstance().snapshot();
    logger.info(
        "收到服务器视觉档案: key={}, netId={:#x}, addr='{}', serverId={}, introLen={}, cached={}",
        *key,
        hash,
        state.remoteString.empty() ? state.remoteAddress : state.remoteString,
        packet.spec.serverId,
        packet.spec.introText.size(),
        saved
    );
}

} // namespace sle::client
