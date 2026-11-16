#pragma once
#include "VanillaUi.h"
#include <memory>
#include <string>
class UIControl;
class ScreenView;
namespace sle::client {
struct NativeMenuState {
    bool playVisible{};
    bool modalVisible{};
    bool focusModal{};
    vanilla::Rect play{};
    float rowStep{32.0f};
    std::string modalName;
};
NativeMenuState inspectMenuTree(std::shared_ptr<UIControl> const& root);
NativeMenuState inspectMenu(ScreenView& view);
std::shared_ptr<UIControl> menuRoot(ScreenView& view);
}
