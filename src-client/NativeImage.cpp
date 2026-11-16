#include "NativeImage.h"

#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/resource/ResourceFileSystem.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace sle::client {

bool drawNativeImage(
    MinecraftUIRenderContext& context, vanilla::Rect rectangle, std::filesystem::path const& path, bool contain
) {
    if (path.empty() || !path.is_absolute() || !std::isfinite(rectangle.width())
        || !std::isfinite(rectangle.height()) || rectangle.width() <= 0 || rectangle.height() <= 0) return false;

    // Path conversion is in-memory; no exists()/file read/network work belongs here.
    auto const utf8 = path.generic_u8string();
    std::string const rawPath{reinterpret_cast<char const*>(utf8.data()), utf8.size()};
    ResourceLocation const location{Core::PathView{rawPath}, ResourceFileSystem::Raw};
    auto const handle = context.getTexture(location, false);
    auto const& texture = handle.mClientTexture.get();
    if (!texture || texture->mIsMissingTexture != IsMissingTexture::No
        || (static_cast<unsigned char>(texture->mTextureLoadState)
            & static_cast<unsigned char>(TextureLoadState::LoadedBit)) == 0) return false;

    auto const& description = texture->mTextureDescription.get();
    float const width = static_cast<float>(description.mWidth);
    float const height = static_cast<float>(description.mHeight);
    if (width <= 0 || height <= 0) return false;
    float u = 0, v = 0, uw = 1, vh = 1;
    float const imageAspect = width / height;
    float const targetAspect = rectangle.width() / rectangle.height();
    if (contain) {
        float const scale = std::min(rectangle.width() / width, rectangle.height() / height);
        float const w = width * scale, h = height * scale;
        rectangle.x0 += (rectangle.width() - w) * 0.5f;
        rectangle.y0 += (rectangle.height() - h) * 0.5f;
        rectangle.x1 = rectangle.x0 + w;
        rectangle.y1 = rectangle.y0 + h;
    } else if (imageAspect > targetAspect) {
        uw = targetAspect / imageAspect;
        u = (1 - uw) * 0.5f;
    } else {
        vh = imageAspect / targetAspect;
        v = (1 - vh) * 0.5f;
    }
    static HashedString const material{"ui_textured_and_color"};
    mce::Color const white{1.0f, 1.0f, 1.0f, 1.0f};
    context.flushImages(white, 1.0f, material);
    context.drawImage(texture->mClientTexture.get(), {rectangle.x0, rectangle.y0},
        {rectangle.width(), rectangle.height()}, {u, v}, {uw, vh}, false);
    context.flushImages(white, 1.0f, material);
    return true;
}

} // namespace sle::client
