#pragma once

#include "VanillaUi.h"
#include <filesystem>

class MinecraftUIRenderContext;

namespace sle::client {

// Render-thread only. A false result means the caller should draw its fallback.
[[nodiscard]] bool drawNativeImage(
    MinecraftUIRenderContext& context,
    vanilla::Rect rectangle,
    std::filesystem::path const& path,
    bool contain = true
);

} // namespace sle::client
