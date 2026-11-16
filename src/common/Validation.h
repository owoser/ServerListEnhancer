#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace sle::validation {

// id 仅允许 [A-Za-z0-9_-]，避免被当成路径片段
[[nodiscard]] bool isValidId(std::string_view id);

// 仅允许 http:// 或 https:// 前缀
[[nodiscard]] bool isValidUrl(std::string_view url);

// 按 UTF-8 字符边界截断，不会切坏多字节字符
[[nodiscard]] std::string truncateUtf8(std::string_view text, std::size_t maxBytes);

// 就地清洗：非法 URL 置空，其余按长度截断；返回 false 表示该字段被丢弃
bool sanitizeUrl(std::string& field);
void sanitizeId(std::string& field);
void sanitizeText(std::string& field, std::size_t maxLength);

} // namespace sle::validation
