#include "ImeCommit.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using sle::android::ime::commit;
using sle::client::TextEdit;

static void check(bool value, char const* label) {
    if (!value) {
        std::cerr << label << " failed\n";
        std::exit(1);
    }
}

int main() {
    {
        // 中文在光标处整段插入，光标落在字符边界。
        std::string value = "ab";
        TextEdit edit;
        edit.end(value);
        edit.left(value, false);
        check(commit(edit, value, "中文", 255), "CJK commit inserts text");
        check(value == "a中文b", "CJK commit keeps caret position");
        check(edit.caret == 1 + 6, "caret advances by UTF-8 bytes");
        check(edit.caret == edit.anchor, "commit collapses the selection");
    }
    {
        // 组合输入控件里的控制字符必须丢弃，不能混进服务器名。
        std::string value;
        TextEdit edit;
        check(commit(edit, value, std::string_view("\x01中\x7f文", 8), 255), "control chars do not block");
        check(value == "中文", "control chars are stripped");
    }
    {
        // 预算不足以放下一个完整字符时整段不写入，避免切坏中文。
        std::string value;
        TextEdit edit;
        check(!commit(edit, value, "中", 2), "limit below one CJK char rejects");
        check(value.empty(), "no partial UTF-8 written");
        check(commit(edit, value, "中", 3), "exact budget accepts");
        check(value == "中", "exact budget writes full char");
    }
    {
        // 空提交与纯控制字符提交不改变正文。
        std::string value = "keep";
        TextEdit edit;
        edit.end(value);
        check(!commit(edit, value, "", 128), "empty text is not consumed");
        check(!commit(edit, value, std::string_view("\x02\x03", 2), 128), "control-only text is not consumed");
        check(value == "keep", "rejected commits leave value untouched");
    }
    {
        // 选中一段再用 IME 提交，整段替换。
        std::string value = "old name";
        TextEdit edit;
        edit.end(value);
        edit.anchor = 0;
        check(commit(edit, value, "服务器", 255), "commit replaces selection");
        check(value == "服务器", "selection replaced by committed text");
    }
    {
        std::string value = "旧名字";
        TextEdit edit;
        edit.end(value);
        edit.anchor = 0;
        check(commit(edit, value, "新名称", 128), "same-byte-length replacement changes text");
        check(value == "新名称", "same-byte-length replacement keeps committed text");
    }
    std::cout << "Android IME commit: CJK insertion, control filtering, UTF-8 budget and selection: PASS\n";
}
