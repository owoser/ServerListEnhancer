#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "TextEdit.h"

namespace sle::android::ime {

// IME 提交的一段 UTF-8 文本（中文等组合输入只在提交时到达）。
// 插入、替换选区和按字符边界截断都交给 TextEdit::insert；这里只过滤空文本，
// 并仅回报正文是否变化；事件是否消费由调用方按当前表单与焦点判断。
inline bool commit(sle::client::TextEdit& editor, std::string& value, std::string_view text, std::size_t limit) {
    if (text.empty()) return false;
    auto const before = value;
    editor.insert(value, text, limit);
    return value != before;
}

} // namespace sle::android::ime
