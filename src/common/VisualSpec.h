#pragma once

#include <cstddef>
#include <string>

namespace sle {

// 线上字段长度上限（服务端裁剪、客户端读取都以此为界，改动需同步 PacketVersion）
inline constexpr std::size_t kMaxIdLength        = 64;
inline constexpr std::size_t kMaxUrlLength       = 512;
inline constexpr std::size_t kMaxIntroTextLength = 1024;

// 服务器视觉档案：服务端下发、客户端渲染的唯一数据结构
struct VisualSpec {
    std::string serverId;      // 卡片归属键，缺省用服务器名
    std::string logoId;        // 内置 logo 资源名（resources/logo/<id>.png）
    std::string logoUrl;       // 远程 logo，空 = 用内置
    std::string introText;     // 简介文本，允许换行
    std::string introImageId;  // 内置简介图资源名
    std::string introImageUrl; // 远程简介图，空 = 用内置
};

} // namespace sle
