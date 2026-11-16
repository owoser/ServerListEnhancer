#pragma once

#include "ClientEntry.h"
#include "SessionTracker.h"
#include "VisualCache.h"
#include "common/ServerVisualPacket.h"

#include "ll/api/io/Logger.h"
#include "ll/api/network/packet/Packet.h"
#include "mc/network/NetworkIdentifier.h"

#include <ctime>
#include <string>
#include <utility>

namespace sle::client {

// 客户端接收 server_visual 包：PacketHandlerBase 在实例化时自动注册到 PacketRegistrar。
// 只负责校验与写缓存，不在渲染线程做任何绘制/IO。
class VisualPacketHandler
: public ll::network::PacketHandlerBase<
      VisualPacketHandler,
      ServerVisualPacket,
      ll::network::PacketSide::Client> {
public:
    void handlePacket(
        NetworkIdentifier const&,
        NetEventCallback&,
        ll::network::PacketSide,
        ServerVisualPacket const& packet
    ) const;
};

} // namespace sle::client
