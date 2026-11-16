#pragma once
#include "VanillaUi.h"
class MinecraftUIRenderContext;
namespace sle::client {
// Same textures, nine-slice sizes and tint as common_buttons.light_text_button.
void drawVanillaButtonSkin(MinecraftUIRenderContext&, vanilla::Rect, vanilla::ButtonState);
}
