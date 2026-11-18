#pragma once

#include <jni.h>
#include <pl/ModMenu.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace sle::android {

struct HomeUiState {
    bool ready{};
    bool home{};
    bool blocked{true};
    float width{};
    float height{};
    std::uint64_t session{};
    std::string screen;
};

// The companion DEX owns the list overlay and observes pass-through home touches; JNI also retains our URI hook.
// joinStatus 接收加入请求的结果：status > 0 表示已经发起深链，<= 0 表示失败并附提示文案。
bool initializeHomeUi(JavaVM* vm, std::filesystem::path modRoot,
                      std::function<void()> open,
                      std::function<bool(float, float, int)> touch,
                      std::function<bool()> back,
                      std::function<void(int, std::string)> joinStatus);
// 让伴生 DEX 在后台解析地址、在主线程发起 LevilLauncher 的 minecraft://connect 深链。
// address 可以是 IPv4 字面量或域名；域名解析失败/超时不会发起任何 Intent。
bool requestServerJoin(std::string address, int port);
// 取消尚未落地的加入请求：过期的 DNS 结果被丢弃，不会再启动游戏或发起连接。
void cancelServerJoin();
HomeUiState homeUiState();
void submitHomeUiFrame(int mode, std::span<const pl::modmenu::DrawCommand> commands, std::uint64_t session = 0);
// Pixels are already decoded, tightly packed RGBA (width * height * 4 bytes).
bool registerHomeUiImage(std::string_view id, std::span<const unsigned char> rgba,
                         int width, int height);
// Width of text as drawn by the overlay (same Paint configuration); negative when JNI is unavailable.
float measureHomeText(std::string_view text, float size);
// Observe raw window-pixel events without consuming them; Java confirms the rendered home button on UP.
void observeHomeUiTouch(float x, float y, int action, int pointerId);
void closeHomeUi();

} // namespace sle::android
