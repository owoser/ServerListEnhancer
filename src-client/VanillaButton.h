#pragma once
#include "VanillaUi.h"
class MinecraftUIRenderContext;
namespace sle::client {
// Same textures, nine-slice sizes and tint as common_buttons.light_text_button.
// alpha 用于整层淡入淡出（0..1），同时作用于贴图 tint 与像素回退路径。
void drawVanillaButtonSkin(MinecraftUIRenderContext&, vanilla::Rect, vanilla::ButtonState, float alpha = 1.0f);
}
