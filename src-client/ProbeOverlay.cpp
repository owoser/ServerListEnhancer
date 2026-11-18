#include "ProbeOverlay.h"

#include "NativeMenuState.h"
#include "VanillaButton.h"
#include "mc/client/gui/Font.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ClientEntry.h"
#include "ServerStatus.h"
#include "ImageProvider.h"
#include "NativeImage.h"
#include "common/Endpoint.h"
#include "AddressParser.h"
#include "TextEdit.h"
#include "LatencyProbe.h"
#include "common/UdpProbeProtocol.h"
#include "ServerStore.h"
#include "ClientConfig.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/service/TargetedBedrock.h"

#include "JoinService.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/FontHandle.h"
#include "mc/client/gui/controls/UIMeasureStrategy.h"
#include "mc/client/gui/controls/MeasureResult.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/RectangleArea.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

namespace sle::client {

namespace {

using Clock = std::chrono::steady_clock;

using Rect = vanilla::Rect;

constexpr float kButtonH = 26.0f;

enum class Page : int { List = 0, Form = 1, Confirm = 2 };

struct RowHit {
    Rect        row{};
    Rect        edit{};
    Rect        del{};
    std::string id;
};

struct FormField {
    std::string label;
    std::string* value{};
    Rect         rect{};
    bool         numeric{false};
};

// ---------------- 共享状态（渲染线程 / 输入线程）----------------
std::mutex gMutex;

bool  gOpen{false};
// 淡入淡出：打开/关闭面板时用 180ms 的透明度过渡，避免整块面板瞬间蹦出。
float gFade{0.0f};                                  // 0..1 线性进度，渲染线程独占
float gUiAlpha{1.0f};                               // 本帧实际使用的透明度（smoothstep 之后）
std::chrono::steady_clock::time_point gFadeAt{};    // 上一帧时间，用于按时间推进
Page  gPage{Page::List};
std::string gSelectedId;
int   gScrollRow{0};
bool  gButtonValid{};
Rect  gButton{};
Rect  gPanel{};
bool  gPanelValid{};

std::vector<RowHit> gRows;
Rect                gAddHit{};
Rect                gRefreshHit{};
Rect                gCloseHit{};
Rect                gAddBtn{};
Rect                gRefreshBtn{};
Rect                gCloseBtn{};
Rect                gJoinBtn{};
bool                gJoinQueued{};
Rect                gPrevBtn{};
Rect                gNextBtn{};
Rect                gSaveBtn{};
Rect                gCancelBtn{};
Rect                gYesBtn{};
Rect                gNoBtn{};
Rect                gDeleteBtn{};

struct FormDraft {
    std::string address;
    std::string name;
};

FormDraft               gDraft;
std::vector<FormField>  gFields;
std::string             gFormId;
int                     gFormActive{0};
bool                    gFormEditing{false};
std::array<TextEdit,2> gEditors;
std::string gFormError;
Clock::time_point gCaretAt=Clock::now();
float gGuiScale=1.0f;
bool gInputForeground{};
std::array<Clock::time_point,256> gKeyRepeatAt{};

std::string gConfirmText;
int         gConfirmId{-1};


std::string       gToast;
Clock::time_point gToastAt{};

float gMouseX{};
float gMouseY{};
bool  gMouseSeen{};

std::atomic<bool> gVisible{false};
Clock::time_point gLastClick{};
Clock::time_point gLastMenuFrame{};


ll::event::ListenerPtr gRenderListener{};
ll::event::ListenerPtr gMouseListener{};
ll::event::ListenerPtr gKeyListener{};

auto& probeLogger() { return ClientEntry::getInstance().getSelf().getLogger(); }

std::string truncateUtf8(std::string const& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return text;
    }
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return text.substr(0, cut) + "...";
}

void fillRect(MinecraftUIRenderContext& ctx, Rect const& r, mce::Color const& color, float alpha) {
    RectangleArea area{};
    area._x0 = std::round(r.x0*gGuiScale)/gGuiScale;
    area._x1 = std::round(r.x1*gGuiScale)/gGuiScale;
    area._y0 = std::round(r.y0*gGuiScale)/gGuiScale;
    area._y1 = std::round(r.y1*gGuiScale)/gGuiScale;
    ctx.fillRectangle(area, color, alpha * gUiAlpha);
}

// ctx.drawText 只是把绘制命令排队，真正提交在 flushText；文本缓冲必须活到那时，
// 否则临时 std::string 析构后渲染出乱码（本项目踩过：dyn 文本全部变 Ö/æñT）。
// JSON UI logical units; GUI scaling is already performed by the game.
float gU = 1.0f;

thread_local std::deque<std::string> gTextKeep;
thread_local MinecraftUIRenderContext* gMeasureContext{};
thread_local FontHandle* gMeasureFont{};
thread_local std::unordered_map<std::string,float> gWidthCache;
float textWidth(Font& font,std::string_view text){
 if(text.empty())return 0;auto key=std::string(text);if(auto it=gWidthCache.find(key);it!=gWidthCache.end())return it->second;
 float width=static_cast<float>(font.getLineLength(text,1.f,false));
 if(gMeasureContext&&gMeasureFont){TextMeasureData data{1.f,0.f,false,false,false,ui::TextAlignment::Left};CaretMeasureData caret{0,false};
  Bedrock::NotNullNonOwnerPtr<FontHandle const> ptr{Bedrock::NonOwnerPointer<FontHandle const>(*gMeasureFont)};
  auto measured=gMeasureContext->getMeasureStrategy().measureTextWidth(ptr,key,data,caret);
  if(std::isfinite(measured.mSize.get().x)&&measured.mSize.get().x>0)width=measured.mSize.get().x;
 }
 gWidthCache.emplace(std::move(key),width);return width;
}

void drawText(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    Rect const&               r,
    std::string_view          text,
    mce::Color const&         color,
    float                     fontSize,
    ui::TextAlignment         alignment = ui::TextAlignment::Left
) {
    gTextKeep.emplace_back(text);
    std::string_view stable{gTextKeep.back()};
    RectangleArea area{};
    area._x0 = std::round(r.x0*gGuiScale)/gGuiScale;
    area._x1 = std::round(r.x1*gGuiScale)/gGuiScale;
    area._y0 = std::round(r.y0*gGuiScale)/gGuiScale;
    area._y1 = std::round(r.y1*gGuiScale)/gGuiScale;

    TextMeasureData  textData{fontSize, 0.0f, false, false, false, alignment};
    CaretMeasureData caretData{0, false};
    mce::Color           faded{color};
    faded.a *= gUiAlpha;
    ctx.drawText(font, area, stable, faded, 1.0f, alignment, textData, caretData);
}

mce::Color color8(int r,int g,int b,float alpha=1.0f){return mce::Color(r/255.f,g/255.f,b/255.f,alpha);}

void drawVanillaButton(
    MinecraftUIRenderContext& ctx, Font& font, Rect const& r,
    std::string_view label, bool hot, float fontSize = 1.0f
) {
    bool const pressed = hot && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    auto const state = !hot ? vanilla::ButtonState::Normal
                           : pressed ? vanilla::ButtonState::Pressed : vanilla::ButtonState::Hover;
    drawVanillaButtonSkin(ctx, r, state, gUiAlpha);
    // Same default font, no shadow, natural line box centered in the button.
    float const height = font.getWrapHeight() * fontSize;
    float const y = std::floor((r.y0 + r.y1 - height) * 0.5f) + (pressed ? 1.0f : 0.0f);
    Rect const textRect{r.x0 + 3.0f, y, r.x1 - 3.0f, y + height};
    float const shade = hot ? 1.0f : 0.3f;
    drawText(ctx, font, textRect, label, mce::Color{shade,shade,shade,1.0f}, fontSize, ui::TextAlignment::Center);
}

// 原版绿色按钮（开始游玩同款贴图）：常态即高亮绿，按下转深绿，文字为白色。
void drawGreenButton(
    MinecraftUIRenderContext& ctx, Font& font, Rect const& r,
    std::string_view label, bool hot, bool enabled = true, float fontSize = 1.0f
) {
    bool const pressed = hot && enabled && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (enabled) drawVanillaButtonSkin(ctx, r, pressed ? vanilla::ButtonState::Pressed : vanilla::ButtonState::Hover, gUiAlpha);
    else drawVanillaButtonSkin(ctx, r, vanilla::ButtonState::Normal, gUiAlpha);
    float const height = font.getWrapHeight() * fontSize;
    float const y = std::floor((r.y0 + r.y1 - height) * 0.5f) + (pressed ? 1.0f : 0.0f);
    Rect const textRect{r.x0 + 3.0f, y, r.x1 - 3.0f, y + height};
    float const shade = !enabled ? 0.45f : hot ? 1.0f : 0.94f;
    drawText(ctx, font, textRect, label, mce::Color{shade,shade,shade,1.0f}, fontSize, ui::TextAlignment::Center);
}

std::string statusKey(SavedServer const& s) { return endpointKey(s.address,s.port); }
void refreshServers(bool force=false) {
    for (auto const& e:ServerStore::getInstance().list())
        ServerStatus::getInstance().request(statusKey(e),e.address,e.port,force);
}
DiscoveryVisual resolvedVisual(SavedServer const& e, std::optional<ServerStatusResult> const& status) {
    DiscoveryVisual v;
    if (status && status->visual) v=*status->visual;
    if (!e.logoUrl.empty()) v.logo={"url",e.logoUrl};
    else if (!e.logoId.empty()) v.logo={"builtin",e.logoId};
    if (!e.introText.empty()) v.introText=e.introText;
    if (!e.introImageUrl.empty()) v.introImage={"url",e.introImageUrl};
    return v;
}
void renderImage(MinecraftUIRenderContext& ctx,Rect r,ImageReference const& ref,bool banner=false) {
    auto& images=ImageProvider::getInstance();
    images.request(ref,banner); // enqueue only; never download or decode on this thread
    auto animation=images.animation(ref,banner);
    // 图片与面板一起淡入淡出：把整层 alpha 交给贴图绘制，避免只有文字和底板在渐变。
    float const alpha=gUiAlpha;
    if(animation&&drawAnimatedImage(ctx,r,animation,true,alpha))return;
    auto path=images.get(ref,banner);
    if(path && drawNativeImage(ctx,r,*path,true,alpha)) return;
    (void)drawNativeImage(ctx,r,images.builtinPath(banner),true,alpha);
}

// 行内小按钮（编辑 / 删除）
bool drawSmallButton(
    MinecraftUIRenderContext& ctx, Font& font, Rect const& r,
    std::string_view label, float mx, float my, bool mouseSeen
) {
    bool const hot = mouseSeen && r.contains(mx, my);
    drawVanillaButton(ctx, font, r, label, hot, 0.85f);
    return hot;
}

void openForm(std::optional<SavedServer> const& existing) {
    std::lock_guard lock(gMutex);
    gFormId     = existing ? existing->id : std::string{};
    gFormActive = 0;
    gFormEditing = true;
    gPage       = Page::Form;
    gSelectedId.clear();
}

} // namespace

// ------------------------------------------------------------------
// 渲染
// ------------------------------------------------------------------
namespace {

Rect inflate(Rect r, float pad) {
    return Rect{r.x0 - pad, r.y0 - pad, r.x1 + pad, r.y1 + pad};
}

void drawMenuButton(MinecraftUIRenderContext& ctx, Font& font, Rect const& button, float mx, float my, bool mouseSeen) {
    {
        std::lock_guard lock(gMutex);
        gButton = button;
        gButtonValid = true;
    }
    drawVanillaButton(ctx, font, button, "服务器列表", mouseSeen && button.contains(mx, my));
}

std::string fitText(Font& font,std::string text,float width){
 if(textWidth(font,text)<=width)return text;
 while(!text.empty()&&textWidth(font,text+"...")>width)text.resize(TextEdit::prev(text,text.size()));
 return text+"...";
}
void border(MinecraftUIRenderContext& ctx,Rect r,mce::Color c){
 fillRect(ctx,{r.x0,r.y0,r.x1,r.y0+1},c,1);fillRect(ctx,{r.x0,r.y1-1,r.x1,r.y1},c,1);
 fillRect(ctx,{r.x0,r.y0,r.x0+1,r.y1},c,1);fillRect(ctx,{r.x1-1,r.y0,r.x1,r.y1},c,1);
}
void drawListPage(MinecraftUIRenderContext& ctx,Font& font,float sw,float sh,float mx,float my,bool mouseSeen){
 float margin=std::max(12.f,sw*.055f);
 Rect panel{margin,sh*.06f,sw-margin,sh*.94f};
 fillRect(ctx,panel,color8(24,24,24),.80f);border(ctx,panel,color8(112,112,112));
 fillRect(ctx,{panel.x0+1,panel.y0+1,panel.x1-1,panel.y0+38},color8(48,48,48),.92f);
 drawText(ctx,font,{panel.x0+14,panel.y0+14,panel.x1-260,panel.y0+32},"服务器列表",color8(255,255,255),1.f);
 gRefreshBtn={panel.x1-244,panel.y0+7,panel.x1-158,panel.y0+31};
 gAddBtn={panel.x1-150,panel.y0+7,panel.x1-14,panel.y0+31};
 drawVanillaButton(ctx,font,gRefreshBtn,"刷新",mouseSeen&&gRefreshBtn.contains(mx,my));
 drawVanillaButton(ctx,font,gAddBtn,"添加服务器",mouseSeen&&gAddBtn.contains(mx,my));
 bool wide=sw>=1000.f;
 float contentTop=panel.y0+48, bottom=panel.y1-40;
 Rect list{panel.x0+12,contentTop,wide?panel.x0+panel.width()*.60f:panel.x1-12,wide?bottom-46.f:bottom-std::clamp(sh*.25f,100.f,160.f)-46.f};
 Rect detail=wide?Rect{list.x1+12,contentTop,panel.x1-12,bottom}:Rect{list.x0,list.y1+46,list.x1,bottom};
 auto entries=ServerStore::getInstance().list();
 int visible=std::max(1,static_cast<int>(list.height()/66.f)),maxScroll=std::max(0,static_cast<int>(entries.size())-visible);
 std::lock_guard lock(gMutex);
 gPanel=panel;gPanelValid=true;gRows.clear();gScrollRow=std::clamp(gScrollRow,0,maxScroll);
 if(std::none_of(entries.begin(),entries.end(),[](auto const& e){return e.id==gSelectedId;}))gSelectedId.clear();
 auto selected=std::find_if(entries.begin(),entries.end(),[](auto const& e){return e.id==gSelectedId;});
 for(int i=0;i<visible&&i+gScrollRow<static_cast<int>(entries.size());++i){
  auto const& e=entries[i+gScrollRow];bool addressValid=validServerEndpoint(e.address,e.port);auto status=ServerStatus::getInstance().get(statusKey(e));auto visual=resolvedVisual(e,status);
  Rect row{list.x0,list.y0+i*66.f,list.x1,list.y0+i*66.f+62};
  bool selected=e.id==gSelectedId,hot=mouseSeen&&row.contains(mx,my);
  fillRect(ctx,row,selected?color8(55,91,43):hot?color8(91,91,91):color8(64,64,64),.92f);
  border(ctx,row,selected?color8(128,207,93):hot?color8(240,240,240):color8(106,106,106));
  renderImage(ctx,{row.x0+7,row.y0+7,row.x0+55,row.y0+55},visual.logo);
  Rect edit{row.x1-36,row.y0+1,row.x1-1,row.y1-1};
  fillRect(ctx,edit,color8(edit.contains(mx,my)?102:76,edit.contains(mx,my)?102:76,edit.contains(mx,my)?102:76),.96f);
  fillRect(ctx,{edit.x0,edit.y0,edit.x0+1,edit.y1},color8(120,120,120),1);
  // Pixel pencil icon, not a tiny scaled text button.
  for(int j=0;j<8;++j)fillRect(ctx,{edit.x0+12+j,edit.y0+33-j,edit.x0+15+j,edit.y0+36-j},color8(238,238,238),1);
  gRows.push_back({row,edit,{},e.id});
  float left=row.x0+64, right=edit.x0-8;
  std::string name=!e.name.empty()?e.name:status&&!status->name.empty()?status->name:statusKey(e);
  drawText(ctx,font,{left,row.y0+7,right-100,row.y0+20},fitText(font,name,right-left-105),color8(255,255,255),1);
  if(addressValid)LatencyProbe::getInstance().request(statusKey(e),e.address,e.port,visual.udpProbePort);
  auto latency=LatencyProbe::getInstance().get(statusKey(e));
  int ms=latency?latency->ms:-1;
  auto ping=!addressValid?std::string("地址无效"):ms>=0?(ms>=1000?std::string("1000+ ms"):std::to_string(ms)+" ms"):(latency&&latency->pending?"UDP 查询中":"UDP 未响应");
  auto pingColor=udp::band(ms)==1?color8(129,230,94):udp::band(ms)==2?color8(255,222,86):udp::band(ms)==3?color8(255,127,115):color8(202,202,202);
  drawText(ctx,font,{right-110,row.y0+25,right,row.y0+38},ping,pingColor,1,ui::TextAlignment::Right);
  std::string population=!addressValid?std::string("请编辑地址"):status&&status->ok?std::to_string(status->players)+"/"+std::to_string(status->maxPlayers):status&&status->state==ServerStatusState::Loading?"查询中":status&&status->state==ServerStatusState::Queued?"排队中":"状态未知";
  drawText(ctx,font,{right-96,row.y0+7,right,row.y0+20},population,color8(235,235,235),1,ui::TextAlignment::Right);
  auto intro=visual.introText.empty()?(status?status->level:""):visual.introText;intro=intro.substr(0,intro.find_first_of("\r\n"));
  drawText(ctx,font,{left,row.y0+25,right-118,row.y0+38},fitText(font,intro,right-left-118),color8(220,220,220),1);
  drawText(ctx,font,{left,row.y0+43,right-115,row.y0+56},fitText(font,statusKey(e),right-left-120),color8(205,205,205),1);
  auto version=status&&!status->version.empty()?status->version:"版本未知";
  drawText(ctx,font,{right-110,row.y0+43,right,row.y0+56},version,color8(225,225,225),1,ui::TextAlignment::Right);
 }
 // 选中服务器后才出现“加入游戏”：绿色原版按钮，点击直连。
 gJoinBtn=Rect{};
 if(selected!=entries.end()){
  bool const addressValid=validServerEndpoint(selected->address,selected->port);
  float const bw=std::min(180.f,list.width()*.42f),bh=26.f,cx=(list.x0+list.x1)*.5f;
  float const by=list.y1+10.f;
  gJoinBtn={cx-bw*.5f,by,cx+bw*.5f,by+bh};
  bool const hot=mouseSeen&&gJoinBtn.contains(mx,my);
  drawGreenButton(ctx,font,gJoinBtn,gJoinQueued?"正在连接...":"加入游戏",hot,addressValid&&!gJoinQueued);
 }
 fillRect(ctx,detail,color8(30,30,30),.65f);border(ctx,detail,color8(96,96,96));
 if(selected!=entries.end()){
  auto status=ServerStatus::getInstance().get(statusKey(*selected));auto visual=resolvedVisual(*selected,status);
  Rect image=wide?Rect{detail.x0+8,detail.y0+8,detail.x1-8,detail.y0+std::min(detail.height()*.48f,detail.width()*.6f)}:Rect{detail.x0+7,detail.y0+7,detail.x0+detail.width()*.46f,detail.y1-7};
  renderImage(ctx,image,visual.introImage,true);
  Rect text=wide?Rect{detail.x0+10,image.y1+10,detail.x1-10,detail.y1-25}:Rect{image.x1+12,detail.y0+10,detail.x1-10,detail.y1-20};
  auto intro=visual.introText.empty()?"欢迎来到本服务器":visual.introText;
  drawText(ctx,font,text,intro,color8(244,244,244),1.f);
  if(status&&status->state==ServerStatusState::Failed)drawText(ctx,font,{text.x0,detail.y1-17,text.x1,detail.y1-4},"暂时无法更新，正在显示缓存",color8(218,218,218),1);
 }else drawText(ctx,font,{detail.x0+14,detail.y0+16,detail.x1-14,detail.y0+30},"选择服务器查看详细信息",color8(235,235,235),1);
 gPrevBtn={panel.x0+12,panel.y1-31,panel.x0+40,panel.y1-7};gNextBtn={panel.x0+46,panel.y1-31,panel.x0+74,panel.y1-7};
 drawVanillaButton(ctx,font,gPrevBtn,"<",mouseSeen&&gPrevBtn.contains(mx,my));drawVanillaButton(ctx,font,gNextBtn,">",mouseSeen&&gNextBtn.contains(mx,my));
 drawText(ctx,font,{panel.x0+84,panel.y1-24,panel.x1-140,panel.y1-9},std::to_string(entries.size())+" 台服务器",color8(230,230,230),1);
 gCloseBtn={panel.x1-114,panel.y1-31,panel.x1-12,panel.y1-7};drawVanillaButton(ctx,font,gCloseBtn,"返回",mouseSeen&&gCloseBtn.contains(mx,my));
}
void drawFormPage(MinecraftUIRenderContext& ctx,Font& font,float sw,float sh,float mx,float my,bool mouseSeen){
 std::lock_guard lock(gMutex);
 float w=std::min(530.f,sw-32),h=std::min(310.f,sh-30);
 Rect panel{(sw-w)/2,(sh-h)/2,(sw+w)/2,(sh+h)/2};gPanel=panel;gPanelValid=true;
 fillRect(ctx,panel,color8(35,35,35),.88f);border(ctx,panel,color8(150,150,150));
 fillRect(ctx,{panel.x0+1,panel.y0+1,panel.x1-1,panel.y0+37},color8(60,60,60),.95f);
 drawText(ctx,font,{panel.x0+14,panel.y0+13,panel.x1-14,panel.y0+29},gFormEditing?"编辑服务器":"添加服务器",color8(255,255,255),1);
 for(std::size_t i=0;i<gFields.size();++i){
  auto& field=gFields[i];float y=panel.y0+54+i*72.f;drawText(ctx,font,{panel.x0+16,y,panel.x1-16,y+12},field.label,color8(240,240,240),1);
  Rect box{panel.x0+16,y+18,panel.x1-16,y+48};field.rect=box;
  fillRect(ctx,box,color8(22,22,22),.94f);border(ctx,box,gFormActive==int(i)?color8(169,218,142):color8(132,132,132));
  auto& edit=gEditors[i];auto const& value=*field.value;edit.clamp(value);
  float available=box.width()-20;
  if(edit.view>edit.caret)edit.view=edit.caret;
  while(edit.view<edit.caret&&textWidth(font,std::string_view(value).substr(edit.view,edit.caret-edit.view))>available-3)edit.view=TextEdit::next(value,edit.view);
  while(edit.view&&textWidth(font,std::string_view(value).substr(TextEdit::prev(value,edit.view),edit.caret-TextEdit::prev(value,edit.view)))<available-15)edit.view=TextEdit::prev(value,edit.view);
  auto tail=value.substr(edit.view);while(!tail.empty()&&textWidth(font,tail)>available)tail.resize(TextEdit::prev(tail,tail.size()));
  float ty=box.y0+(box.height()-font.getWrapHeight())/2;
  auto a=std::max(edit.view,std::min(edit.caret,edit.anchor)),b=std::min(edit.view+tail.size(),std::max(edit.caret,edit.anchor));
  if(gFormActive==int(i)&&b>a){float x0=box.x0+9+textWidth(font,std::string_view(value).substr(edit.view,a-edit.view)),x1=box.x0+9+textWidth(font,std::string_view(value).substr(edit.view,b-edit.view));fillRect(ctx,{x0,ty-1,x1,ty+font.getWrapHeight()+1},color8(70,113,142),1);}
  if(value.empty())drawText(ctx,font,{box.x0+9,ty,box.x1-9,box.y1},i==0?"例如 play.example.com:19132":"可选，留空使用服务器名称",color8(168,168,168),1);
  else drawText(ctx,font,{box.x0+9,ty,box.x1-9,box.y1},tail,color8(255,255,255),1);
  if(gFormActive==int(i)&&gInputForeground&&std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-gCaretAt).count()%1000<500){float x=box.x0+9+textWidth(font,std::string_view(value).substr(edit.view,edit.caret-edit.view));fillRect(ctx,{x,ty-1,x+1,ty+font.getWrapHeight()+1},color8(255,255,255),1);}
 }
 drawText(ctx,font,{panel.x0+16,panel.y1-96,panel.x1-16,panel.y1-76},gFormError.empty()?"保存后自动获取图文 · Tab 切换 · Ctrl+V 粘贴":gFormError,gFormError.empty()?color8(222,222,222):color8(255,154,140),1);
 float by=panel.y1-43;
 gDeleteBtn=gFormEditing?Rect{panel.x0+16,by,panel.x0+108,by+26}:Rect{};
 if(gFormEditing)drawVanillaButton(ctx,font,gDeleteBtn,"删除服务器",mouseSeen&&gDeleteBtn.contains(mx,my));
 gSaveBtn={panel.x1-218,by,panel.x1-116,by+26};gCancelBtn={panel.x1-108,by,panel.x1-16,by+26};
 drawVanillaButton(ctx,font,gSaveBtn,"保存",mouseSeen&&gSaveBtn.contains(mx,my));drawVanillaButton(ctx,font,gCancelBtn,"取消",mouseSeen&&gCancelBtn.contains(mx,my));
}

void drawConfirmPage(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    float                     sw,
    float                     sh,
    float                     mx,
    float                     my,
    bool                      mouseSeen
) {
    Rect box{sw * 0.34f, sh * 0.34f, sw * 0.66f, sh * 0.62f};
    std::lock_guard lock(gMutex);
    gPanel=box; gPanelValid=true;
    fillRect(ctx, box, color8(34, 34, 40), 0.99f);
    border(ctx,box,color8(150,150,150));
    drawText(
        ctx,
        font,
        Rect{box.x0 + 16.0f, box.y0 + 18.0f, box.x1 - 16.0f, box.y0 + 60.0f},
        std::string_view{gConfirmText},
        color8(240, 240, 244),
        1.0f,
        ui::TextAlignment::Center
    );
    float by = box.y1 - 44.0f;
    gYesBtn      = Rect{box.x0 + 20.0f, by, box.x0 + 20.0f + 120.0f, by + kButtonH * gU};
    gNoBtn       = Rect{box.x1 - 140.0f, by, box.x1 - 20.0f, by + kButtonH * gU};
    drawVanillaButton(ctx, font, gYesBtn, std::string_view{"确定"}, mouseSeen && gYesBtn.contains(mx, my));
    drawVanillaButton(ctx, font, gNoBtn, std::string_view{"取消"}, mouseSeen && gNoBtn.contains(mx, my));
}

void drawToast(MinecraftUIRenderContext& ctx, Font& font, float sw, float sh) {
    std::string toast;
    {
        std::lock_guard lock(gMutex);
        if (gToast.empty()) {
            return;
        }
        if (Clock::now() - gToastAt > std::chrono::seconds(4)) {
            gToast.clear();
            return;
        }
        toast = gToast;
    }
    Rect box{sw * 0.5f - 180.0f, sh * 0.06f, sw * 0.5f + 180.0f, sh * 0.06f + 30.0f};
    fillRect(ctx, box, color8(20, 20, 24), 0.92f);
    drawText(
        ctx,
        font,
        Rect{box.x0 + 8.0f, box.y0 + 7.0f, box.x1 - 8.0f, box.y1},
        std::string_view{toast},
        color8(235, 235, 240),
        1.0f,
        ui::TextAlignment::Center
    );
}

// Key snapshots are updated even for suppressed chords: Alt+A cannot leak A later.
std::array<bool,256> gKeyPrev{};

// ESC 在覆盖层里只用于返回：吞掉原版按键，避免同时弹出暂停/退出对话框。
std::atomic<long long> gEscSuppressMs{0};

long long steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string clipboardText(){
 std::string out;if(!OpenClipboard(nullptr))return out;
 if(HANDLE data=GetClipboardData(CF_UNICODETEXT))if(auto w=static_cast<wchar_t*>(GlobalLock(data))){
  int n=WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
  if(n>1){out.resize(n);WideCharToMultiByte(CP_UTF8,0,w,-1,out.data(),n,nullptr,nullptr);out.pop_back();}GlobalUnlock(data);
 }
 CloseClipboard();return out;
}
void setClipboard(std::string const& text){
 int n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
 if(n<0||!OpenClipboard(nullptr))return;auto block=GlobalAlloc(GMEM_MOVEABLE,(n+1)*sizeof(wchar_t));
 if(block){auto w=static_cast<wchar_t*>(GlobalLock(block));MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),w,n);w[n]=0;GlobalUnlock(block);EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,block))GlobalFree(block);}CloseClipboard();
}
SavedServer draftToServer();
bool saveDraft(){
 auto parsed=parseServerAddress(gDraft.address);
 if(!parsed){gFormError=parsed.error;gCaretAt=Clock::now();return false;}
 auto srv=draftToServer();gFormId=ServerStore::getInstance().upsert(srv);
 ServerStore::getInstance().save();ServerStatus::getInstance().request(statusKey(srv),srv.address,srv.port,true);
 gJoinBtn={};gFormError.clear();gPage=Page::List;return true;
}
void pollKeyboard(){
 DWORD pid{};GetWindowThreadProcessId(GetForegroundWindow(),&pid);
 bool foreground=pid==GetCurrentProcessId();
 std::array<bool,256> down{},edge{};
 for(int i=0;i<256;++i){down[i]=(GetAsyncKeyState(i)&0x8000)!=0;edge[i]=down[i]&&!gKeyPrev[i];}gKeyPrev=down;
 bool resumed=foreground&&!gInputForeground;gInputForeground=foreground;
 if(!foreground||resumed)return;
 std::lock_guard lock(gMutex);if(!gOpen)return;
 if(down[VK_MENU]||down[VK_LWIN]||down[VK_RWIN]){gKeyRepeatAt={};return;}
 if(edge[VK_ESCAPE]){gEscSuppressMs.store(steadyMs()+250);if(gPage==Page::List)gOpen=false;else gPage=Page::List;return;}
 if(gPage!=Page::Form)return;
 bool ctrl=down[VK_CONTROL],shift=down[VK_SHIFT];
 if(edge[VK_TAB]){gFormActive=(gFormActive+1)%2;gCaretAt=Clock::now();return;}
 if(edge[VK_RETURN]&&!ctrl){saveDraft();return;}
 auto& edit=gEditors[gFormActive];auto& value=gFormActive==0?gDraft.address:gDraft.name;edit.clamp(value);
 if(ctrl){
  if(edge['A']){edit.anchor=0;edit.caret=value.size();}
  else if(edge['C']||edge['X']){if(edit.selected())setClipboard(edit.selectedText(value));if(edge['X'])edit.eraseSelection(value);}
  else if(edge['V'])edit.insert(value,clipboardText(),gFormActive?128:255);
  gCaretAt=Clock::now();return;
 }
 auto keyNow=Clock::now();
 for(int vk:{VK_LEFT,VK_RIGHT,VK_BACK,VK_DELETE}){if(!down[vk])gKeyRepeatAt[vk]={};else if(edge[vk])gKeyRepeatAt[vk]=keyNow+std::chrono::milliseconds(400);else if(gKeyRepeatAt[vk]!=Clock::time_point{}&&keyNow>=gKeyRepeatAt[vk]){edge[vk]=true;gKeyRepeatAt[vk]=keyNow+std::chrono::milliseconds(45);}}
 bool changed=false;
 if(edge[VK_LEFT]){edit.left(value,shift);changed=true;}
 if(edge[VK_RIGHT]){edit.right(value,shift);changed=true;}
 if(edge[VK_HOME]){edit.caret=0;if(!shift)edit.anchor=0;changed=true;}
 if(edge[VK_END]){edit.caret=value.size();if(!shift)edit.anchor=edit.caret;changed=true;}
 if(edge[VK_BACK]){edit.backspace(value);changed=true;}
 if(edge[VK_DELETE]){edit.del(value);changed=true;}
 BYTE state[256]{};GetKeyboardState(state);
 for(int vk=32;vk<256;++vk){if(!edge[vk]||vk==VK_DELETE||vk==VK_LEFT||vk==VK_RIGHT||vk==VK_HOME||vk==VK_END)continue;
  wchar_t chars[8]{};int count=ToUnicode(vk,MapVirtualKeyW(vk,MAPVK_VK_TO_VSC),state,chars,8,0);
  if(count>0){char utf8[32]{};int n=WideCharToMultiByte(CP_UTF8,0,chars,count,utf8,32,nullptr,nullptr);if(n>0){edit.insert(value,std::string_view(utf8,n),gFormActive?128:255);changed=true;}}
 }
 if(changed){gCaretAt=Clock::now();gFormError.clear();}
}

bool queryCursorClient(int& outX, int& outY);

void drawProbe(ll::event::render::AfterUIRenderEvent& event) {
    auto& ctx = event.uiRenderContext();
    auto& view = event.screenView();
    // AfterUIRenderEvent fires for EACH ScreenView, not once per frame. Drawing
    // again on toast/debug was the source of duplicated dark overlay layers.
    if (view.getScreenName() != "start_screen") {
        if(Clock::now()-gLastMenuFrame>std::chrono::milliseconds(250)){endImageFrame(false);ImageProvider::getInstance().tick(false);LatencyProbe::getInstance().pause();}
        return;
    }
    auto ci = ll::service::bedrock::getClientInstance();
    if (!ci) { gVisible.store(false); return; }
    auto const size = view.mSize.get();
    float const sw = size.x, sh = size.y;
    auto const stack = ci->getCurrentSceneStack();
    bool sceneAllowed = true;
    for (auto const& name : stack->getScreenNames()) {
        if (name != "start_screen" && name != "toast_screen" && name != "debug_screen"
            && name != "cubemap_background_screen") sceneAllowed = false;
    }
    auto const menu = inspectMenu(view);
    bool const allowed = vanilla::allowOverlay(sceneAllowed, menu.playVisible, menu.modalVisible, menu.focusModal)
                         && sw > 1.0f && sh > 1.0f;
    static std::string lastState;
    auto const state = std::string(allowed ? "visible" : "hidden") + ":" + menu.modalName
        + (menu.playVisible ? ":play" : ":no-play");
    if (state != lastState) {
        lastState=state;
        if (debugLogging()) {
            probeLogger().info("[UI/native-v2] {} modal={} focusModal={} play=({},{} {}x{}) step={}",
                state, menu.modalVisible, menu.focusModal, menu.play.x0,menu.play.y0,
                menu.play.width(),menu.play.height(),menu.rowStep);
        }
    }
    gVisible.store(allowed);
    {
        std::lock_guard lock(gMutex);
        gLastMenuFrame = Clock::now();
        if (!allowed) {
            // Hide AND invalidate hit targets: a native popup must receive all
            // input. Never let invisible stale hitboxes consume its buttons.
            gButtonValid=false; gPanelValid=false; gRows.clear();
        }
    }
    if (!allowed) {endImageFrame(false);ImageProvider::getInstance().tick(false);LatencyProbe::getInstance().pause();return;}
    beginImageFrame();
    FontHandle handle = ci->getFontHandle();
    auto font=handle.mDefaultFont;
    if (!font) { gVisible.store(false); return; }
    // Keep the game's logical pixel size. Window resize/GUI-scale changes are
    // already reflected in the Play control geometry and the UI coordinate space.
    gU=1.0f;
    gGuiScale=std::max(1.f,ci->getGuiScale());
    auto const entry = vanilla::menuEntry(menu.play,menu.rowStep);
    int px=0, py=0;
    if (queryCursorClient(px,py)) {
        float const scale=ci->getGuiScale();
        if (scale>0.0f) { std::lock_guard lock(gMutex); gMouseX=px/scale; gMouseY=py/scale; gMouseSeen=true; }
    }

    gTextKeep.clear();gWidthCache.clear();gMeasureContext=&ctx;gMeasureFont=&handle;

    float mx{}, my{};
    bool  mouseSeen{};
    bool  open{};
    Page  page{Page::List};
    {
        std::lock_guard lock(gMutex);
        mx        = gMouseX;
        my        = gMouseY;
        mouseSeen = gMouseSeen;
        open      = gOpen;
        page      = gPage;
    }

    pollKeyboard();
    {std::lock_guard lock(gMutex);open=gOpen;page=gPage;}

    // 180ms 淡入淡出：按真实帧间隔推进，再用 smoothstep 让起止都不生硬。
    auto const nowTick = Clock::now();
    float      dt      = 0.0f;
    if (gFadeAt.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(nowTick - gFadeAt).count();
    }
    gFadeAt = nowTick;
    dt      = std::clamp(dt, 0.0f, 0.1f);
    float const target = open ? 1.0f : 0.0f;
    float const step   = dt / 0.18f;
    gFade = gFade < target ? std::min(target, gFade + step) : std::max(target, gFade - step);
    float const eased = gFade * gFade * (3.0f - 2.0f * gFade);

    // Publish visibility once, including the fading frames. Releasing decoded
    // GIFs before drawing and marking them visible again afterwards races workers.
    bool const listVisibleNow = (open || gFade > 0.002f) && page == Page::List;
    ImageProvider::getInstance().tick(listVisibleNow);
    if (open || gFade > 0.002f) {
        // 关闭过程中仍按剩余透明度重绘一帧帧，直到完全透明。
        gUiAlpha = eased;
        if (page == Page::List) {
            drawListPage(ctx, *font, sw, sh, mx, my, mouseSeen);
        } else if (page == Page::Form) {
            drawFormPage(ctx, *font, sw, sh, mx, my, mouseSeen);
        } else {
            drawConfirmPage(ctx, *font, sw, sh, mx, my, mouseSeen);
        }
    } else {
        gUiAlpha = 1.0f;
        drawMenuButton(ctx, *font, entry, mx, my, mouseSeen);
    }

    drawToast(ctx, *font, sw, sh);
    ctx.flushText(0.0f, std::nullopt);
    gMeasureContext=nullptr;gMeasureFont=nullptr;
    // CPU frames and GPU textures share the same visibility through the fade.
    endImageFrame(listVisibleNow);
    static bool wasListVisible{};
    bool const listVisible=open&&page==Page::List;
    if(wasListVisible&&!listVisible&&debugLogging())probeLogger().info("[GIF] list hidden, decodedBytes={}",gifMemoryBytes());
    wasListVisible=listVisible;
    if(!listVisible)LatencyProbe::getInstance().pause();
}

// ------------------------------------------------------------------
// 输入
// ------------------------------------------------------------------
void respondError(std::string const& text) {
    std::lock_guard lock(gMutex);
    gToast   = text;
    gToastAt = Clock::now();
}

void rebuildFields() {
    gFields.clear();
    gFields.push_back({"地址", &gDraft.address, {}, false});
    gFields.push_back({"名称", &gDraft.name, {}, false});
}


void startAddForm() {
    std::lock_guard lock(gMutex);
    gDraft       = FormDraft{};
    gFormId      = {};
    gFormActive  = 0;
    gFormEditing = false;
    gFormError.clear();gEditors={};gCaretAt=Clock::now();
    rebuildFields();
    gPage = Page::Form;
    if (debugLogging()) probeLogger().info("[UI] form opened (add)");
}

// 列表页“加入游戏”：取当前选中条目，交给原版 joiner 直连。
void joinSelected() {
    std::string id;
    {
        std::lock_guard lock(gMutex);
        if (!gOpen || gPage != Page::List || gJoinQueued) return;
        id = gSelectedId;
    }
    auto entries = ServerStore::getInstance().list();
    auto it      = std::find_if(entries.begin(), entries.end(), [&](auto const& e) { return e.id == id; });
    if (it == entries.end()) {
        if (debugLogging()) probeLogger().info("[UI] join: no selection");
        return;
    }
    if (!validServerEndpoint(it->address, it->port)) {
        probeLogger().warn("[UI] join: invalid address '{}'", it->address);
        respondError("地址无效，请先编辑服务器");
        return;
    }
    auto status = ServerStatus::getInstance().get(statusKey(*it));
    auto name = !it->name.empty() ? it->name : status && !status->name.empty() ? status->name : statusKey(*it);
    { std::lock_guard lock(gMutex); gJoinQueued = true; }
    (void)sle::client::joinServer(it->address, it->port, name, [](bool submitted, std::string const& message) {
        std::lock_guard lock(gMutex);
        gJoinQueued = false;
        gJoinBtn = {};
        if (submitted) {
            gOpen = false;
            gPanelValid = false;
            gButtonValid = false;
            gRows.clear();
        } else {
            gToast = message;
            gToastAt = Clock::now();
        }
    });
}

SavedServer draftToServer() {
    SavedServer srv;
    if(auto existing=ServerStore::getInstance().get(gFormId))srv=*existing;
    auto const previous=statusKey(srv);
    srv.id   = gFormId;
    srv.name = gDraft.name;
    auto parsed=parseServerAddress(gDraft.address);
    srv.address=parsed.host;srv.port=parsed.port;
    if(previous!=statusKey(srv))ServerStatus::getInstance().invalidate(previous);
    return srv;
}

// 从窗口取真实光标位置（cancel 事件后 MouseInputEvent 的坐标会冻结在旧值）
bool queryCursorClient(int& outX, int& outY) {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        return false;
    }
    wchar_t title[128]{};
    GetWindowTextW(hwnd, title, 128);
    if (std::wstring_view(title).find(L"Minecraft") == std::wstring_view::npos) {
        return false;
    }
    POINT pt{};
    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt)) {
        return false;
    }
    outX = pt.x;
    outY = pt.y;
    return true;
}

void onMouse(ll::event::input::MouseInputEvent& event) {
    int  cursorX = 0;
    int  cursorY = 0;
    bool haveCursor = queryCursorClient(cursorX, cursorY);

    static std::atomic<int> sMouseLogs{0};
    int  rawAction = static_cast<int>(event.actionButtonId());
    int  rawButton = static_cast<int>(event.buttonData());
    if (debugLogging() && (rawAction != 0 || sMouseLogs.load() < 5)) {
        if (sMouseLogs.load() < 40) {
            sMouseLogs.fetch_add(1);
            probeLogger().info(
                "[UI] mouse raw=({}, {}) cursor=({}, {}) btn={} action={} visible={}",
                event.x(),
                event.y(),
                haveCursor ? cursorX : -1,
                haveCursor ? cursorY : -1,
                rawButton,
                rawAction,
                gVisible.load()
            );
        }
    }
    if (!gVisible.load()) return;
    {
        std::lock_guard lock(gMutex);
        if (Clock::now() - gLastMenuFrame > std::chrono::milliseconds(250)) return;
    }
    auto ci = ll::service::bedrock::getClientInstance();
    if (!ci) {
        return;
    }
    float guiScale = ci->getGuiScale();
    if (!(guiScale > 0.01f)) {
        guiScale = 1.0f;
    }

    // MouseInputEvent coordinates can be relative while the menu uses absolute
    // client pixels. Match the cursor source used to draw hover feedback.
    float uiX = static_cast<float>(haveCursor ? cursorX : event.x()) / guiScale;
    float uiY = static_cast<float>(haveCursor ? cursorY : event.y()) / guiScale;

    bool openNow{};
    {
        std::lock_guard lock(gMutex);
        gMouseX    = uiX;
        gMouseY    = uiY;
        gMouseSeen = true;
        openNow    = gOpen;
    }
    // 面板打开时，仅在面板区域内拦截鼠标，避免穿透点到被遮住的原版按钮
    if (openNow) {
        Rect panel{};
        bool panelValid{};
        {
            std::lock_guard lock(gMutex);
            panel      = gPanel;
            panelValid = gPanelValid;
        }
        if (panelValid && panel.contains(uiX, uiY)) {
            event.cancel();
        }
    }

    int const buttonId = static_cast<int>(event.buttonData());
    int const actionId = static_cast<int>(event.actionButtonId());
    if (actionId != 1 || buttonId != 1) return; // left press only
    auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - gLastClick).count() < 200) {
        return;
    }
    gLastClick = now;

    bool  open{};
    Page  page{Page::List};
    Rect  button{};
    bool  buttonValid{};
    std::vector<RowHit> rows;
    Rect  addBtn{}, refreshBtn{}, closeBtn{}, prevBtn{}, nextBtn{}, saveBtn{}, cancelBtn{}, yesBtn{}, noBtn{}, joinBtn{};
    Rect  addHit{}, refreshHit{}, closeHit{}, deleteBtn{};
    std::vector<FormField> fields;
    std::string            formId;
    {
        std::lock_guard lock(gMutex);
        open        = gOpen;
        page        = gPage;
        button      = gButton;
        buttonValid = gButtonValid;
        rows        = gRows;
        addBtn      = gAddBtn;
        refreshBtn  = gRefreshBtn;
        closeBtn    = gCloseBtn;
        joinBtn     = gJoinBtn;
        addHit      = gAddHit;
        refreshHit  = gRefreshHit;
        closeHit    = gCloseHit;
        prevBtn     = gPrevBtn;
        nextBtn     = gNextBtn;
        saveBtn     = gSaveBtn;
        cancelBtn   = gCancelBtn;
        deleteBtn   = gDeleteBtn;
        yesBtn      = gYesBtn;
        noBtn       = gNoBtn;
        fields      = gFields;
        formId      = gFormId;
    }

    if (debugLogging()) probeLogger().info("[UI] click ui=({}, {}) page={} open={}", uiX, uiY, static_cast<int>(page), open ? 1 : 0);

    if (!open) {
        if (buttonValid && button.contains(uiX, uiY)) {
            event.cancel();
            refreshServers();
            std::lock_guard lock(gMutex);
            gOpen = true;
            gPage = Page::List;
            gSelectedId.clear();
            gJoinBtn = {};
            gJoinQueued = false;
        }
        return;
    }

    if (page == Page::List) {
        if (closeBtn.contains(uiX, uiY)) {
            if (debugLogging()) probeLogger().info("[UI] hit close");
            std::lock_guard lock(gMutex);
            gOpen = false;
            return;
        }
        if (joinBtn.width() > 0 && joinBtn.contains(uiX, uiY)) {
            if (debugLogging()) probeLogger().info("[UI] hit join");
            joinSelected();
            return;
        }
        if (addBtn.contains(uiX, uiY)) {
            if (debugLogging()) probeLogger().info("[UI] hit add");
            startAddForm();
            return;
        }
        if (refreshBtn.contains(uiX, uiY)) {
            auto entries = ServerStore::getInstance().list();
            for (auto const& e : entries) {
                ServerStatus::getInstance().request(statusKey(e), e.address, e.port, true);
            }
            respondError("已触发状态刷新");
            return;
        }
        if (prevBtn.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gScrollRow = std::max(0, gScrollRow - 1);
            gSelectedId.clear(); gJoinBtn = {};
            return;
        }
        if (nextBtn.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gScrollRow += 1;
            gSelectedId.clear(); gJoinBtn = {};
            return;
        }
        for (auto const& row : rows) {
            if (row.del.contains(uiX, uiY)) {
                std::lock_guard lock(gMutex);
                gPage        = Page::Confirm;
                gConfirmId   = 0;
                gConfirmText = "确定删除这条服务器？";
                static std::string targetId;
                targetId = row.id;
                gFormId  = targetId;
                return;
            }
            if (row.edit.contains(uiX, uiY)) {
                auto srv = ServerStore::getInstance().get(row.id);
                if (!srv) {
                    continue;
                }
                std::lock_guard lock(gMutex);
                gDraft.name      = srv->name;
                gDraft.address   = displayAddress(srv->address,srv->port);
                gEditors[0].end(gDraft.address);gEditors[1].end(gDraft.name);gFormError.clear();gCaretAt=Clock::now();
                rebuildFields();
                gFormId      = srv->id;
                gFormActive  = 0;
                gFormEditing = true;
                gPage        = Page::Form;
                return;
            }
            if (row.row.contains(uiX, uiY)) {
                std::lock_guard lock(gMutex);
                gSelectedId=row.id;
                return;
            }
        }
        return;
    }

    if (page == Page::Confirm) {
        if (yesBtn.contains(uiX, uiY)) {
            std::string id;
            {
                std::lock_guard lock(gMutex);
                id = gFormId;
            }
            if (!id.empty()) {
                if(auto e=ServerStore::getInstance().get(id))ServerStatus::getInstance().invalidate(statusKey(*e));
                ServerStore::getInstance().remove(id);

                ServerStore::getInstance().save();
            }
            std::lock_guard lock(gMutex);
            gPage = Page::List;
            return;
        }
        if (noBtn.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gPage = Page::Form;
            return;
        }
        return;
    }

    // Form
    if(deleteBtn.width()>0&&deleteBtn.contains(uiX,uiY)){std::lock_guard lock(gMutex);gPage=Page::Confirm;gConfirmText="确定删除这条服务器？";return;}
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].rect.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gFormActive = static_cast<int>(i);
            auto& edit=gEditors[i];auto const& text=*fields[i].value;edit.clamp(text);
            auto ci=ll::service::bedrock::getClientInstance();auto handle=ci->getFontHandle();auto font=handle.mDefaultFont;
            if(font){std::size_t pos=edit.view;float target=uiX-fields[i].rect.x0-9;
                while(pos<text.size()){auto next=TextEdit::next(text,pos);float left=font->getLineLength(std::string_view(text).substr(edit.view,pos-edit.view),1,false),right=font->getLineLength(std::string_view(text).substr(edit.view,next-edit.view),1,false);if(target<(left+right)/2)break;pos=next;}
                edit.caret=edit.anchor=pos;
            }gCaretAt=Clock::now();
            return;
        }
    }
    if(saveBtn.contains(uiX,uiY)){std::lock_guard lock(gMutex);saveDraft();return;}
    if (cancelBtn.contains(uiX, uiY)) {
        std::lock_guard lock(gMutex);
        gPage = Page::List;
        return;
    }
}

} // namespace

void registerProbeOverlay() {
    probeLogger().info("[UI/native-v2] vanilla textures + native Play geometry + inline modal guard");
    auto& bus = ll::event::EventBus::getInstance();
    if (!gRenderListener) {
        gRenderListener = bus.emplaceListener<ll::event::render::AfterUIRenderEvent>(
            [](ll::event::render::AfterUIRenderEvent& event) { drawProbe(event); }
        );
    }
    if (!gMouseListener) {
        gMouseListener =
            bus.emplaceListener<ll::event::input::MouseInputEvent>([](ll::event::input::MouseInputEvent& event) {
                onMouse(event);
            });
    }
    if (!gKeyListener) {
        gKeyListener = bus.emplaceListener<ll::event::input::KeyInputEvent>(
            [](ll::event::input::KeyInputEvent& event) {
                if (!event.isDown() || event.keyCode() != VK_ESCAPE) return;
                bool openNow = false;
                {
                    std::lock_guard lock(gMutex);
                    openNow = gOpen;
                }
                // 覆盖层打开，或刚被这次 ESC 关闭（淡出中）时，都不让原版看到该键。
                if (openNow || steadyMs() < gEscSuppressMs.load()) event.cancel();
            }
        );
    }

}

void unregisterProbeOverlay() {
    {std::lock_guard lock(gMutex);gOpen=false;gVisible=false;gButtonValid=false;gPanelValid=false;gRows.clear();gJoinBtn={};gJoinQueued=false;gSelectedId.clear();}
    auto& bus = ll::event::EventBus::getInstance();
    if (gRenderListener) {
        bus.removeListener(gRenderListener);
        gRenderListener.reset();
    }
    if (gMouseListener) {
        bus.removeListener(gMouseListener);
        gMouseListener.reset();
    }
    if (gKeyListener) {
        bus.removeListener(gKeyListener);
        gKeyListener.reset();
    }
    ImageProvider::getInstance().tick(false);
    endImageFrame(false);
}

} // namespace sle::client