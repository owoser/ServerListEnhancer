#include "VanillaButton.h"

#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/resource/ResourceFileSystem.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"

namespace sle::client {
namespace {
void fill(MinecraftUIRenderContext& ctx, vanilla::Rect r, mce::Color color, float alpha) {
    RectangleArea area{};
    area._x0=r.x0; area._y0=r.y0; area._x1=r.x1; area._y1=r.y1;
    ctx.fillRectangle(area, color, alpha);
}
bool texture(MinecraftUIRenderContext& ctx, vanilla::Rect r, std::string_view path, float sourceSize, mce::Color tint, float alpha) {
    ResourceLocation const location{Core::PathView{path}, ResourceFileSystem::UserPackage};
    auto handle = ctx.getTexture(location, false);
    auto const& data = handle.mClientTexture.get();
    if (!data || data->mIsMissingTexture != IsMissingTexture::No
        || (static_cast<unsigned char>(data->mTextureLoadState) & static_cast<unsigned char>(TextureLoadState::LoadedBit)) == 0)
        return false;
    for (auto const& slice : vanilla::nineSlice(r, sourceSize)) {
        if (slice.target.width() <= 0 || slice.target.height() <= 0) continue;
        ctx.drawImage(data->mClientTexture.get(),
            {slice.target.x0, slice.target.y0}, {slice.target.width(), slice.target.height()},
            {slice.uv.x0, slice.uv.y0}, {slice.uv.width(), slice.uv.height()}, false);
    }
    // Isolate the tint from previous draw batches. Vanilla UI textures are not
    // linearized on the CPU; doing so caused the dark grey/green regression.
    static HashedString const material{"ui_textured_and_color"};
    ctx.flushImages(tint, alpha, material);
    return true;
}
}

void drawVanillaButtonSkin(MinecraftUIRenderContext& ctx, vanilla::Rect root, vanilla::ButtonState state, float alpha) {
    auto const index = static_cast<std::size_t>(state);
    auto const image = vanilla::inset(root, 1.0f);
    if (!texture(ctx, image, vanilla::ButtonTextures[index], 4.0f, mce::Color{1.0f,1.0f,1.0f,1.0f}, alpha)) {
        auto const slices = vanilla::nineSlice(image, 4.0f);
        for (std::size_t i=0; i<slices.size(); ++i) {
            auto const p=vanilla::ButtonPixels[index][i];
            fill(ctx, slices[i].target, mce::Color{vanilla::channel(p.r),vanilla::channel(p.g),vanilla::channel(p.b),1.0f}, alpha);
        }
    }
    // common_buttons.light_button_assets uses the SAME full-size white-border
    // texture in every state, tinted .075 in normal and white in hover/press.
    float const border = state == vanilla::ButtonState::Normal ? 0.075f : 1.0f;
    mce::Color const tint{border,border,border,1.0f};
    if (!texture(ctx, root, "textures/ui/focus_border_white", 5.0f, tint, alpha)) {
        auto const slices = vanilla::nineSlice(root, 5.0f);
        for (std::size_t i=0; i<slices.size(); ++i) if(i != 4) fill(ctx, slices[i].target, tint, alpha);
    }
}
} // namespace sle::client
