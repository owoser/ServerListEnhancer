#include "common/Validation.h"

#include "common/VisualSpec.h"

namespace sle::validation {

bool isValidId(std::string_view id) {
    if (id.empty() || id.size() > kMaxIdLength) {
        return false;
    }
    for (char const c : id) {
        bool const ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
                     || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool isValidUrl(std::string_view url) {
    if (url.empty() || url.size() > kMaxUrlLength) {
        return false;
    }
    auto const startsWith = [url](std::string_view prefix) {
        return url.size() >= prefix.size() && url.compare(0, prefix.size(), prefix) == 0;
    };
    if (!startsWith("http://") && !startsWith("https://")) {
        return false;
    }
    for (char const c : url) {
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) {
            return false; // 拒绝空白与控制字符，避免请求头注入
        }
    }
    return true;
}

std::string truncateUtf8(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return std::string{text};
    }
    std::size_t end = maxBytes;
    // 回退到 UTF-8 字符边界：跳过所有续字节 (10xxxxxx)
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string{text.substr(0, end)};
}

void sanitizeId(std::string& field) {
    if (field.empty()) {
        return;
    }
    if (!isValidId(field)) {
        field.clear();
        return;
    }
}

bool sanitizeUrl(std::string& field) {
    if (field.empty()) {
        return true;
    }
    if (!isValidUrl(field)) {
        field.clear();
        return false;
    }
    return true;
}

void sanitizeText(std::string& field, std::size_t maxLength) { field = truncateUtf8(field, maxLength); }

} // namespace sle::validation
