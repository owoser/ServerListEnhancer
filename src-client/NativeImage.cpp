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
#include <vector>
#include <cstring>
#include <mutex>
#include "ClientEntry.h"
#include "ll/api/io/Logger.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core_graphics/ImageBuffer.h"

namespace sle::client {

bool drawNativeImage(
    MinecraftUIRenderContext& context, vanilla::Rect rectangle, std::filesystem::path const& path, bool contain, float alpha
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
    context.flushImages(white, alpha, material);
    context.drawImage(texture->mClientTexture.get(), {rectangle.x0, rectangle.y0},
        {rectangle.width(), rectangle.height()}, {u, v}, {uw, vh}, false);
    context.flushImages(white, 1.0f, material);
    return true;
}

namespace {
using Clock=std::chrono::steady_clock;
struct AnimationTexture {
 std::shared_ptr<GifAnimation const> animation;
 std::shared_ptr<mce::TextureGroup> group;
 std::string path;
 Clock::time_point last{Clock::now()};
 Clock::duration elapsed{};
 std::size_t frame{~std::size_t{}};
 unsigned updates{};
 bool seen{};
};
std::mutex textureMutex;
std::vector<AnimationTexture> textures;
std::uint64_t textureSequence{};
}
void beginImageFrame(){std::lock_guard lock(textureMutex);for(auto& t:textures)t.seen=false;}
void endImageFrame(bool visible){
 std::lock_guard lock(textureMutex);
 auto now=Clock::now();
 for(auto it=textures.begin();it!=textures.end();){
  if(!visible||(!it->seen&&now-it->last>std::chrono::seconds(2))){
   if(it->group)it->group->unloadTexture(ResourceLocation{Core::PathView{it->path},ResourceFileSystem::Raw},false);
   auto path=it->path;
   it=textures.erase(it);
   ClientEntry::getInstance().getSelf().getLogger().info("[GIF] released texture={}, remaining={}, cpuBytes={}",path,textures.size(),gifMemoryBytes());
  }else ++it;
 }
}
bool drawAnimatedImage(MinecraftUIRenderContext& ctx,vanilla::Rect rect,std::shared_ptr<GifAnimation const> const& animation,bool contain,float alpha){
 if(!animation||animation->frames.empty())return false;
 std::lock_guard lock(textureMutex);
 auto now=Clock::now();
 auto it=std::find_if(textures.begin(),textures.end(),[&](auto const& t){return t.animation==animation;});
 if(it==textures.end()){
  auto ci=ll::service::bedrock::getClientInstance();if(!ci)return false;
  auto group=ci->getTextureGroup();if(!group)return false;
  std::size_t textureBytes{};for(auto const& t:textures)textureBytes+=std::size_t(t.animation->width)*t.animation->height*4;
  if(textures.size()>=8||textureBytes+std::size_t(animation->width)*animation->height*4>32u*1024*1024)return false;
  AnimationTexture t;t.animation=animation;t.group=std::move(group);
  t.path="sle_animation_"+std::to_string(++textureSequence);textures.push_back(std::move(t));it=std::prev(textures.end());
  ClientEntry::getInstance().getSelf().getLogger().info("[GIF] texture={}, {}x{}, frames={}, live={}, cpuBytes={}",it->path,animation->width,animation->height,animation->frames.size(),textures.size(),gifMemoryBytes());
 }
 auto& t=*it;
 auto delta=now-t.last;
 // A long absence is a pause, not a request to catch up through invisible frames.
 if(delta<std::chrono::milliseconds(250))t.elapsed+=delta;
 t.last=now;t.seen=true;
 auto frame=animation->frameAt(std::chrono::duration_cast<std::chrono::milliseconds>(t.elapsed));
 ResourceLocation location{Core::PathView{t.path},ResourceFileSystem::Raw};
 if(frame!=t.frame){
  mce::Image image(animation->width,animation->height,mce::ImageFormat::RGBA8Unorm,mce::ImageUsage::SRGB);
  image.mAlphaUsage=mce::AlphaUsage::Transparent;
  image.resizeImageBytesToFitImageDescription();
  auto const& pixels=animation->frames[frame];
  if(image.mImageBytes.size()!=pixels.size())return false;
  std::memcpy(image.mImageBytes.data(),pixels.data(),pixels.size());
  cg::ImageBuffer buffer(std::move(image));
  if(t.frame==~std::size_t{})t.group->uploadTexture(location,std::move(buffer));
  else if(!t.group->updateTextureInPlace(location,std::move(buffer)))return false;
  t.frame=frame;
  if(++t.updates==3)ClientEntry::getInstance().getSelf().getLogger().info("[GIF] playback confirmed texture={}, frame={}, uploads={}",t.path,frame,t.updates);
 }
 auto handle=ctx.getTexture(location,false);auto const& texture=handle.mClientTexture.get();if(!texture)return false;
 if(contain){auto scale=std::min(rect.width()/animation->width,rect.height()/animation->height);auto w=scale*animation->width,h=scale*animation->height;rect.x0+=(rect.width()-w)/2;rect.y0+=(rect.height()-h)/2;rect.x1=rect.x0+w;rect.y1=rect.y0+h;}
 static HashedString material("ui_textured_and_color");mce::Color white{1.f,1.f,1.f,1.f};
 ctx.flushImages(white,alpha,material);ctx.drawImage(texture->mClientTexture.get(),{rect.x0,rect.y0},{rect.width(),rect.height()},{0,0},{1,1},false);ctx.flushImages(white,alpha,material);
 return true;
}

void resetAnimatedTextures(){
 // 退出世界后客户端的纹理组会失效或被复用，旧贴图再也更新不上，表现就是 GIF 停在首帧。
 std::lock_guard lock(textureMutex);
 std::size_t const dropped=textures.size();
 textures.clear();
 ClientEntry::getInstance().getSelf().getLogger().info("[GIF] textures reset on level exit, dropped={}",dropped);
}
} // namespace sle::client
