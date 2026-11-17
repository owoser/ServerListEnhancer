#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
namespace sle::udp {
inline constexpr int PacketSize=32;
inline constexpr std::array<unsigned char,8> Request{'S','L','E','U','D','P','1','?'};
inline constexpr std::array<unsigned char,8> Response{'S','L','E','U','D','P','1','!'};
inline int band(int ms){return ms<0?0:ms<=60?1:ms<=100?2:3;}
}
