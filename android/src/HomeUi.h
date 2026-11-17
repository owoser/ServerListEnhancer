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

// The companion DEX owns the list overlay view, and the JNI bridge intercepts only our home-entry URI.
bool initializeHomeUi(JavaVM* vm, std::filesystem::path modRoot,
                      std::function<void()> open,
                      std::function<bool(float, float, int)> touch,
                      std::function<bool()> back);
HomeUiState homeUiState();
void submitHomeUiFrame(int mode, std::span<const pl::modmenu::DrawCommand> commands, std::uint64_t session = 0);
// Pixels are already decoded, tightly packed RGBA (width * height * 4 bytes).
bool registerHomeUiImage(std::string_view id, std::span<const unsigned char> rgba,
                         int width, int height);
void closeHomeUi();

} // namespace sle::android
