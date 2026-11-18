#pragma once

#include "VanillaUi.h"
#include <filesystem>
#include "GifAnimation.h"

class MinecraftUIRenderContext;

namespace sle::client {

// Render-thread only. A false result means the caller should draw its fallback.
[[nodiscard]] bool drawNativeImage(
    MinecraftUIRenderContext& context,
    vanilla::Rect rectangle,
    std::filesystem::path const& path,
    bool contain = true,
    float alpha = 1.0f
);

void beginImageFrame();
void endImageFrame(bool visible);
// 世界切换会让缓存贴图挂在失效的纹理组上，退出世界时清空全部动画贴图。
void resetAnimatedTextures();
bool drawAnimatedImage(MinecraftUIRenderContext&,vanilla::Rect,std::shared_ptr<GifAnimation const> const&,bool contain=true,float alpha=1.0f);
} // namespace sle::client
