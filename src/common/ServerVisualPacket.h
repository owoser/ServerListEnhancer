#pragma once

#include "common/VisualSpec.h"

#include "ll/api/network/packet/Packet.h"
#include "mc/deps/core/utility/BinaryStream.h"
#include "mc/deps/core/utility/ReadOnlyBinaryStream.h"
#include "mc/platform/Result.h"

#include <string>
#include <utility>

namespace sle {

// 服务端 -> 客户端的视觉档案包。
// Optional{true}：未安装客户端模组的玩家不受影响（配合 Packet::canSendTo 使用）。
class ServerVisualPacket : public ll::network::PacketBase<ServerVisualPacket> {
public:
    static constexpr std::string_view        CustomPacketId{"server_list_enhancer:server_visual"};
    static constexpr uint                    PacketVersion{1};
    static constexpr ll::network::PacketSide Side{ll::network::PacketSide::Client};
    static constexpr bool                    Optional{true};

    VisualSpec spec;

    void write(BinaryStream& stream) const override {
        stream.writeString(spec.serverId, nullptr, nullptr);
        stream.writeString(spec.logoId, nullptr, nullptr);
        stream.writeString(spec.logoUrl, nullptr, nullptr);
        stream.writeString(spec.introText, nullptr, nullptr);
        stream.writeString(spec.introImageId, nullptr, nullptr);
        stream.writeString(spec.introImageUrl, nullptr, nullptr);
    }

    Bedrock::Result<void> read(ReadOnlyBinaryStream& stream) override {
        if (auto r = readField(stream, kMaxIdLength, spec.serverId); !r) {
            return r;
        }
        if (auto r = readField(stream, kMaxIdLength, spec.logoId); !r) {
            return r;
        }
        if (auto r = readField(stream, kMaxUrlLength, spec.logoUrl); !r) {
            return r;
        }
        if (auto r = readField(stream, kMaxIntroTextLength, spec.introText); !r) {
            return r;
        }
        if (auto r = readField(stream, kMaxIdLength, spec.introImageId); !r) {
            return r;
        }
        return readField(stream, kMaxUrlLength, spec.introImageUrl);
    }

private:
    static Bedrock::Result<void> readField(ReadOnlyBinaryStream& stream, std::size_t limit, std::string& out) {
        auto value = stream.getString(limit);
        if (!value) {
            return nonstd::make_unexpected(std::move(value).error());
        }
        out = std::move(*value);
        return {};
    }
};

} // namespace sle
