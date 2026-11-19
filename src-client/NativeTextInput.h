#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// 透明 Win32 EDIT 持有 IMM 上下文，负责中文候选与组字；调用方始终用自绘字体显示。
// 预览可能包含尚未提交的组字，只能用于显示。无 LeviLamina 依赖，可单独测试。
namespace sle::client {

// 结束编辑的原因；文本随结果一并回传。
enum class NativeTextAction : int {
    Commit = 0, // 回车（非组字）/ 主动提交
    Tab    = 1, // Tab：提交并请求切到下一个字段
    Cancel = 2, // Esc（非组字）/ 外部关闭；text 回传打开时的原文
    Blur   = 3, // 失去焦点：提交当前文本
};

struct NativeTextResult {
    std::uint64_t    session{};
    int              field{};
    NativeTextAction action{NativeTextAction::Commit};
    std::string      text;
};

// 打开（或就地切换字段）原生输入框。
// owner     游戏窗口，用作弹窗的 owner，保证层级与最小化跟随
// screenBox 目标区域，屏幕坐标（像素）
// text      初始文本（UTF-8）
// maxBytes  UTF-8 字节上限，超出按码点边界截断
// fontPixelHeight 与原字段文字一致的像素字号（<=0 时按框高回退）
// textInsetPixels 与原字段一致的左内边距（像素）
// session  调用方会话号，原样回传
// field    调用方字段号，原样回传
// 返回 false 表示工作线程不可用；同一时间只维护一个输入窗。
bool openNativeTextInput(
    HWND             owner,
    RECT             screenBox,
    std::string const& text,
    std::size_t      maxBytes,
    std::uint64_t    session,
    int              field,
    int              fontPixelHeight = 0,
    int              textInsetPixels = 4
);

// 窗口移动/缩放/滚动时更新位置；未激活时忽略。
void positionNativeTextInput(RECT screenBox);

// 是否有正在编辑的原生输入框。
bool nativeTextInputActive();

// 编辑过程中的实时快照（含 IME 组字文本），供调用方用自绘字体呈现。
struct NativeTextPreview {
    std::uint64_t session{};
    int           field{};
    std::string   text;
    int           caret{};  // UTF-8 字节偏移
    int           anchor{}; // UTF-8 字节偏移（选区另一端）
};
std::optional<NativeTextPreview> takeNativeTextPreview();

// 取走一次编辑结果（线程安全，取走即清空；多次结束编辑按产生顺序排队）。
std::optional<NativeTextResult> takeNativeTextInput();

// 同步结束当前编辑并产出结果（Blur 语义），供“点击游戏背景/保存按钮”前兜底。
// 内部使用带 250ms 超时的 SendMessageTimeout，不会在持锁状态下跨线程发送。
void commitNativeTextInput();

// 程序侧关闭当前输入框，不产生结果、不抢焦点。
void closeNativeTextInput();

// 停止工作线程并清理窗口类；可重复调用。
void shutdownNativeTextInput();

} // namespace sle::client
