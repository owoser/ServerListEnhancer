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
#include "ServerStore.h"
#include "VisualCache.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/FontHandle.h"
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
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
Rect                gPrevBtn{};
Rect                gNextBtn{};
Rect                gSaveBtn{};
Rect                gCancelBtn{};
Rect                gYesBtn{};
Rect                gNoBtn{};

struct FormDraft {
    std::string address;
    std::string name;
};

FormDraft               gDraft;
std::vector<FormField>  gFields;
std::string             gFormId;
int                     gFormActive{0};
bool                    gFormEditing{false};

std::string gConfirmText;
int         gConfirmId{-1};

std::atomic<int>  gLastVk{-1};

std::string       gToast;
Clock::time_point gToastAt{};

float gMouseX{};
float gMouseY{};
bool  gMouseSeen{};

std::atomic<bool> gVisible{false};
std::atomic<int>  gDrawCount{0};
Clock::time_point gLastClick{};
Clock::time_point gLastMenuFrame{};

thread_local bool tTracing = false;

ll::event::ListenerPtr gRenderListener{};
ll::event::ListenerPtr gMouseListener{};
ll::event::ListenerPtr gKeyListener{};

auto& probeLogger() { return ClientEntry::getInstance().getSelf().getLogger(); }

void trace(std::string_view msg) {
    static std::mutex mutex;
    std::lock_guard   lock(mutex);
    auto              path = ClientEntry::getInstance().getSelf().getConfigDir().parent_path() / "probe_trace.log";
    path.make_preferred();
    std::ofstream ofs(path, std::ios::app);
    if (ofs) {
        ofs << msg << '\n';
        ofs.flush();
    }
}

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
    area._x0 = r.x0;
    area._x1 = r.x1;
    area._y0 = r.y0;
    area._y1 = r.y1;
    ctx.fillRectangle(area, color, alpha);
}

// ctx.drawText 只是把绘制命令排队，真正提交在 flushText；文本缓冲必须活到那时，
// 否则临时 std::string 析构后渲染出乱码（本项目踩过：dyn 文本全部变 Ö/æñT）。
// JSON UI logical units; GUI scaling is already performed by the game.
float gU = 1.0f;

thread_local std::deque<std::string> gTextKeep;

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
    area._x0 = r.x0;
    area._x1 = r.x1;
    area._y0 = r.y0;
    area._y1 = r.y1;

    TextMeasureData  textData{fontSize, 0.0f, false, false, false, alignment};
    CaretMeasureData caretData{0, false};
    ctx.drawText(font, area, stable, color, 1.0f, alignment, textData, caretData);
}

mce::Color color8(int r, int g, int b, float alpha = 1.0f) {
    // 管线会把 linear 值编码成 sRGB 输出，所以这里先做 sRGB -> linear 反算
    auto toLinear = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return mce::Color(toLinear(r / 255.0f), toLinear(g / 255.0f), toLinear(b / 255.0f), alpha);
}

void drawVanillaButton(
    MinecraftUIRenderContext& ctx, Font& font, Rect const& r,
    std::string_view label, bool hot, float fontSize = 1.0f
) {
    bool const pressed = hot && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    auto const state = !hot ? vanilla::ButtonState::Normal
                           : pressed ? vanilla::ButtonState::Pressed : vanilla::ButtonState::Hover;
    drawVanillaButtonSkin(ctx, r, state);
    // Same default font, no shadow, natural line box centered in the button.
    float const height = font.getWrapHeight() * fontSize;
    float const y = std::floor((r.y0 + r.y1 - height) * 0.5f) + (pressed ? 1.0f : 0.0f);
    Rect const textRect{r.x0 + 3.0f, y, r.x1 - 3.0f, y + height};
    float const shade = hot ? 1.0f : 0.3f;
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
    auto path=images.get(ref,banner);
    if(path && drawNativeImage(ctx,r,*path,true)) return;
    (void)drawNativeImage(ctx,r,images.builtinPath(banner),true);
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

void drawListPage(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    float                     sw,
    float                     sh,
    float                     mx,
    float                     my,
    bool                      mouseSeen
) {
    bool const html = false; // Keep a single native rendering path.

    float const u = gU;

    Rect panel{sw * 0.18f, sh * 0.10f, sw * 0.82f, sh * 0.92f};
    if (!html) {
        fillRect(ctx, panel, color8(28, 28, 32), 0.97f);
        Rect border{panel.x0 - u, panel.y0 - u, panel.x1 + u, panel.y1 + u};
        fillRect(ctx, border, color8(92, 92, 100), 1.0f);

        Rect header{panel.x0, panel.y0, panel.x1, panel.y0 + 42.0f * u};
        fillRect(ctx, header, color8(38, 38, 43), 1.0f);
        fillRect(ctx, Rect{panel.x0, panel.y0 + 42.0f * u, panel.x1, panel.y0 + 43.0f * u}, color8(88, 88, 96), 0.9f);
        drawText(
            ctx,
            font,
            Rect{panel.x0 + 16.0f * u, panel.y0 + 10.0f * u, panel.x1 - 16.0f * u, panel.y0 + 38.0f * u},
            std::string_view{"服务器列表"},
            color8(255, 255, 255),
            1.5f
        );
    }

    // 数据
    auto entries = ServerStore::getInstance().list();
    auto rowsTop    = panel.y0 + 52.0f * u;
    float const detailH=std::clamp(sh*0.23f,88.0f,130.0f);
    auto rowsBottom = panel.y1 - 66.0f * u - detailH - 10.0f;
    float rowH      = 64.0f * u;
    float rowGap    = 6.0f * u;
    float rowX0     = panel.x0 + 12.0f * u;
    float rowX1     = panel.x1 - 12.0f * u;

    int visible = std::max(1, static_cast<int>((rowsBottom - rowsTop) / (rowH + rowGap)));
    int maxScroll = std::max(0, static_cast<int>(entries.size()) - visible);

    {
        std::lock_guard lock(gMutex);
        gScrollRow = std::clamp(gScrollRow, 0, maxScroll);
        if (!entries.empty() && std::none_of(entries.begin(),entries.end(),[](auto const& e){return e.id==gSelectedId;}))
            gSelectedId=entries.front().id;
        gRows.clear();
        gPanel      = panel;
        gPanelValid = true;

        for (int i = 0; i < visible; ++i) {
            int idx = gScrollRow + i;
            if (idx >= static_cast<int>(entries.size())) {
                break;
            }
            auto&       e = entries[static_cast<std::size_t>(idx)];
            float       y = rowsTop + static_cast<float>(i) * (rowH + rowGap);
            Rect        row{rowX0, y, rowX1, y + rowH};
            bool        hot     = mouseSeen && row.contains(mx, my);
            bool        isSel   = (e.id == gSelectedId);
            RowHit      hit{};
            hit.row = row;
            hit.id  = e.id;
            hit.edit = Rect{rowX1 - 108.0f * u, y + rowH - 24.0f * u, rowX1 - 60.0f * u, y + rowH - 4.0f * u};
            hit.del  = Rect{rowX1 - 54.0f * u, y + rowH - 24.0f * u, rowX1 - 6.0f * u, y + rowH - 4.0f * u};
            gRows.push_back(hit);

            if (html) {
                if (hot || isSel) {
                    fillRect(ctx, row, color8(255, 255, 255), 0.06f);
                }
                drawSmallButton(ctx, font, hit.edit, std::string_view{"编辑"}, mx, my, mouseSeen);
                drawSmallButton(ctx, font, hit.del, std::string_view{"删除"}, mx, my, mouseSeen);
                continue;
            }

            fillRect(
                ctx,
                row,
                isSel ? color8(58, 58, 66) : (hot ? color8(46, 46, 52) : color8(36, 36, 41)),
                1.0f
            );
            fillRect(ctx, Rect{row.x0, row.y0, row.x1, row.y0 + 1.0f}, color8(90, 90, 98), 0.8f);

            auto status = ServerStatus::getInstance().get(statusKey(e));
            auto const visual=resolvedVisual(e,status);
            Rect logo{row.x0+10,y+8,row.x0+58,y+56};
            renderImage(ctx,logo,visual.logo);

            std::string name   = e.name;
            if (name.empty() && status && !status->name.empty()) {
                name = status->name;
            }
            if (name.empty()) {
                name = statusKey(e);
            }
            drawText(
                ctx,
                font,
                Rect{row.x0 + 68.0f, y + 7.0f, row.x1 - 120.0f, y + 27.0f},
                std::string_view{truncateUtf8(name, 64)},
                color8(242, 242, 246),
                1.3f
            );

            std::string intro = visual.introText;
            if (intro.empty() && status && status->ok) {
                intro = status->level;
            }
            if (!intro.empty()) {
                drawText(
                    ctx,
                    font,
                    Rect{row.x0 + 68.0f, y + 26.0f, row.x1 - 120.0f, y + 40.0f},
                    std::string_view{truncateUtf8(intro.substr(0,intro.find_first_of("\r\n")), 90)},
                    color8(176, 176, 184),
                    0.9f
                );
            }

            drawText(
                ctx,
                font,
                Rect{row.x0 + 68.0f, y + 44.0f, row.x1 - 120.0f, y + 58.0f},
                std::string_view{statusKey(e)},
                color8(140, 140, 150),
                0.85f
            );

            // 在线人数
            std::string players = "--";
            if (status) {
                if (status->state==ServerStatusState::Queued) players="排队中";
                else if (status->state==ServerStatusState::Loading) players="查询中";
                else if (status->ok) {
                    players = std::to_string(status->players) + "/" + std::to_string(status->maxPlayers);
                } else {
                    players = "离线";
                }
            }
            drawText(
                ctx,
                font,
                Rect{rowX1 - 118.0f, y + 8.0f, rowX1 - 8.0f, y + 24.0f},
                std::string_view{players},
                color8(196, 196, 204),
                0.9f,
                ui::TextAlignment::Right
            );

            drawSmallButton(ctx, font, hit.edit, std::string_view{"编辑"}, mx, my, mouseSeen);
            drawSmallButton(ctx, font, hit.del, std::string_view{"删除"}, mx, my, mouseSeen);
        }

        // 空列表提示
        if (!html && entries.empty()) {
            drawText(
                ctx,
                font,
                Rect{panel.x0 + 16.0f, rowsTop + 40.0f, panel.x1 - 16.0f, rowsTop + 70.0f},
                std::string_view{"还没有服务器，点「添加」新建一条"},
                color8(170, 170, 180),
                1.2f,
                ui::TextAlignment::Center
            );
        }



        // Selected-server details: native banner, full introduction, endpoint attribution.
        if (!entries.empty()) {
            auto it=std::find_if(entries.begin(),entries.end(),[](auto const& e){return e.id==gSelectedId;});
            if(it!=entries.end()){
                auto const status=ServerStatus::getInstance().get(statusKey(*it));
                auto const visual=resolvedVisual(*it,status);
                Rect const detail{rowX0,rowsBottom+10,rowX1,panel.y1-66};
                fillRect(ctx,detail,color8(36,36,41),1.0f);
                float const w=std::min(detail.width()*0.49f,detail.height()*3.0f);
                renderImage(ctx,{detail.x0+6,detail.y0+6,detail.x0+w-6,detail.y1-6},visual.introImage,true);
                auto intro=visual.introText;
                if(intro.empty()) intro=status&&!status->level.empty()?status->level:"欢迎来到本服务器";
                drawText(ctx,font,{detail.x0+w+8,detail.y0+8,detail.x1-8,detail.y1-24},truncateUtf8(intro,768),color8(235,235,240),0.95f);
                std::string source=status&&status->visual?"已获取服务器图文":"内置默认图片";
                if(status&&status->state==ServerStatusState::Failed)source="查询失败，保留缓存/默认图";
                drawText(ctx,font,{detail.x0+w+8,detail.y1-20,detail.x1-8,detail.y1-4},source,color8(160,180,160),0.8f);
            }
        }

        // 翻页
        gPrevBtn = Rect{panel.x1 - 60.0f, panel.y0 + 8.0f, panel.x1 - 38.0f, panel.y0 + 34.0f};
        gNextBtn = Rect{panel.x1 - 36.0f, panel.y0 + 8.0f, panel.x1 - 14.0f, panel.y0 + 34.0f};
        bool prevHot = mouseSeen && gPrevBtn.contains(mx, my);
        bool nextHot = mouseSeen && gNextBtn.contains(mx, my);
        fillRect(ctx, gPrevBtn, color8(prevHot ? 70 : 54, prevHot ? 70 : 54, prevHot ? 78 : 60), 1.0f);
        fillRect(ctx, gNextBtn, color8(nextHot ? 70 : 54, nextHot ? 70 : 54, nextHot ? 78 : 60), 1.0f);
        drawText(
            ctx,
            font,
            Rect{gPrevBtn.x0, gPrevBtn.y0 + 6.0f, gPrevBtn.x1, gPrevBtn.y1},
            std::string_view{"^"},
            color8(230, 230, 236),
            1.0f,
            ui::TextAlignment::Center
        );
        drawText(
            ctx,
            font,
            Rect{gNextBtn.x0, gNextBtn.y0 + 6.0f, gNextBtn.x1, gNextBtn.y1},
            std::string_view{"v"},
            color8(230, 230, 236),
            1.0f,
            ui::TextAlignment::Center
        );

        // 底部按钮
        float by = panel.y1 - 58.0f * u;
        gRefreshBtn = Rect{panel.x0 + 16.0f, by, panel.x0 + 128.0f, by + kButtonH * gU};
        gAddBtn     = Rect{panel.x0 + 136.0f, by, panel.x0 + 248.0f, by + kButtonH * gU};
        gCloseBtn   = Rect{panel.x1 - 152.0f, by, panel.x1 - 24.0f, by + kButtonH * gU};

        {
            float bandTop    = panel.y1 - 120.0f;
            float bandBottom = panel.y1 - 2.0f;
            float third      = (panel.x1 - panel.x0 - 24.0f) / 3.0f;
            gRefreshHit      = Rect{panel.x0 + 8.0f, bandTop, panel.x0 + 8.0f + third, bandBottom};
            gAddHit          = Rect{panel.x0 + 12.0f + third, bandTop, panel.x0 + 12.0f + third * 2.0f, bandBottom};
            gCloseHit        = Rect{panel.x1 - 8.0f - third, bandTop, panel.x1 - 8.0f, bandBottom};
        }
        drawVanillaButton(ctx, font, gRefreshBtn, std::string_view{"刷新状态"}, mouseSeen && gRefreshBtn.contains(mx, my));
        drawVanillaButton(ctx, font, gAddBtn, std::string_view{"添加服务器"}, mouseSeen && gAddBtn.contains(mx, my));
        drawVanillaButton(ctx, font, gCloseBtn, std::string_view{"关闭"}, mouseSeen && gCloseBtn.contains(mx, my));
    }
}

void drawFormPage(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    float                     sw,
    float                     sh,
    float                     mx,
    float                     my,
    bool                      mouseSeen
) {
    Rect panel{sw * 0.26f, sh * 0.16f, sw * 0.74f, sh * 0.86f};
    fillRect(ctx, panel, color8(30, 30, 35), 0.98f);
    fillRect(ctx, Rect{panel.x0 - 1, panel.y0 - 1, panel.x1 + 1, panel.y1 + 1}, color8(96, 96, 104), 1.0f);
    fillRect(ctx, Rect{panel.x0, panel.y0, panel.x1, panel.y0 + 40.0f}, color8(40, 40, 46), 1.0f);
    drawText(
        ctx,
        font,
        Rect{panel.x0 + 14.0f, panel.y0 + 9.0f, panel.x1 - 14.0f, panel.y0 + 34.0f},
        std::string_view{gFormEditing ? "服务器信息" : "服务器信息"},
        color8(255, 255, 255),
        1.4f
    );

    std::lock_guard lock(gMutex);
    gPanel=panel; gPanelValid=true;
    float           y     = panel.y0 + 54.0f;
    float           fieldH = 34.0f;
    for (std::size_t i = 0; i < gFields.size(); ++i) {
        auto& f = gFields[i];
        Rect  row{panel.x0 + 14.0f, y, panel.x1 - 14.0f, y + fieldH};
        f.rect    = row;
        bool active = static_cast<int>(i) == gFormActive;
        fillRect(ctx, row, active ? color8(52, 52, 60) : color8(38, 38, 43), 1.0f);
        if (mouseSeen && row.contains(mx, my)) {
            fillRect(ctx, Rect{row.x0, row.y0, row.x0 + 3.0f, row.y1}, color8(120, 160, 220), 1.0f);
        }
        drawText(
            ctx,
            font,
            Rect{row.x0 + 10.0f, row.y0 + 8.0f, row.x0 + 120.0f, row.y1},
            std::string_view{f.label},
            color8(205, 205, 215),
            0.95f
        );
        std::string value = f.value ? *f.value : std::string{};
        if (value.empty()) {
            value = active ? "" : "（空）";
        }
        drawText(
            ctx,
            font,
            Rect{row.x0 + 126.0f, row.y0 + 8.0f, row.x1 - 10.0f, row.y1},
            std::string_view{truncateUtf8(value, 120)},
            color8(240, 240, 244),
            1.0f
        );
        y += fieldH + 8.0f;
    }

    drawText(ctx,font,{panel.x0+16,panel.y1-76,panel.x1-16,panel.y1-60},
        "保存后自动查询服务器图文；名称留空使用服务器名称",color8(160,180,160),0.85f);

    float by = panel.y1 - 44.0f;
    gSaveBtn   = Rect{panel.x1 - 300.0f, by, panel.x1 - 160.0f, by + kButtonH * gU};
    gCancelBtn = Rect{panel.x1 - 148.0f, by, panel.x1 - 14.0f, by + kButtonH * gU};
    drawVanillaButton(ctx, font, gSaveBtn, std::string_view{"保存"}, mouseSeen && gSaveBtn.contains(mx, my));
    drawVanillaButton(ctx, font, gCancelBtn, std::string_view{"取消"}, mouseSeen && gCancelBtn.contains(mx, my));
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
    fillRect(ctx, Rect{box.x0 - 1, box.y0 - 1, box.x1 + 1, box.y1 + 1}, color8(110, 110, 120), 1.0f);
    drawText(
        ctx,
        font,
        Rect{box.x0 + 16.0f, box.y0 + 18.0f, box.x1 - 16.0f, box.y0 + 60.0f},
        std::string_view{gConfirmText},
        color8(240, 240, 244),
        1.1f,
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

// ---------------- 键盘轮询（HBUI 抢焦点时 KeyInputEvent 收不到）----------------
std::array<bool, 256> gKeyPrev{};

bool keyEdge(int vk) {
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool was = gKeyPrev[static_cast<std::size_t>(vk)];
    gKeyPrev[static_cast<std::size_t>(vk)] = now;
    if (now && !was) {
        gLastVk.store(vk);
    }
    return now && !was;
}

std::string clipboardText() {
    std::string out;
    if (!OpenClipboard(nullptr)) {
        return out;
    }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* w = static_cast<wchar_t*>(GlobalLock(data))) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                std::string buffer(static_cast<std::size_t>(n - 1), 0);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, buffer.data(), n, nullptr, nullptr);
                out = buffer;
            }
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out;
}

char vkToChar(int vk) {
    if (vk >= 0x30 && vk <= 0x39) {
        return static_cast<char>(0x30 + (vk - 0x30));
    }
    if (vk >= 0x41 && vk <= 0x5A) {
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        return static_cast<char>((shift ? 0x41 : 0x61) + (vk - 0x41));
    }
    switch (vk) {
    case VK_OEM_PERIOD:
        return 0x2E;
    case VK_OEM_COMMA:
        return 0x2C;
    case VK_OEM_MINUS:
        return 0x2D;
    case VK_OEM_PLUS:
        return 0x3D;
    case VK_OEM_1:
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x3A : 0x3B;
    case VK_OEM_2:
        return 0x2F;
    case VK_OEM_3:
        return (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x7E : 0x60;
    case VK_OEM_4:
        return 0x5B;
    case VK_OEM_5:
        return 0x5C;
    case VK_OEM_6:
        return 0x5D;
    case VK_OEM_7:
        return 0x27;
    case VK_SPACE:
        return 0x20;
    default:
        return 0;
    }
}

SavedServer draftToServer();

void pollKeyboard() {
    bool open{};
    Page page{Page::List};
    {
        std::lock_guard lock(gMutex);
        open = gOpen;
        page = gPage;
    }
    if (!open) {
        static std::atomic<int> sNoOpen{0};
        if (sNoOpen.fetch_add(1) < 3) {
            probeLogger().info("[UI] pollKeyboard: 面板未打开");
        }
        return;
    }
    if (page == Page::List || page == Page::Confirm) {
        if (keyEdge(VK_ESCAPE)) {
            std::lock_guard lock(gMutex);
            if (page == Page::Confirm) {
                gPage = Page::List;
            } else {
                gOpen = false;
            }
        }
        return;
    }

    std::lock_guard lock(gMutex);
    if (keyEdge(VK_ESCAPE)) {
        gPage = Page::List;
        return;
    }
    if (keyEdge(VK_TAB)) {
        if (!gFields.empty()) {
            gFormActive = (gFormActive + 1) % static_cast<int>(gFields.size());
        }
        return;
    }
    if (keyEdge(VK_RETURN)) {
        auto srv = draftToServer();
        if (srv.address.empty()) {
            gToast   = "地址不能为空";
            gToastAt = Clock::now();
            return;
        }
        gFormId = ServerStore::getInstance().upsert(srv);

        ServerStore::getInstance().save();
        ServerStatus::getInstance().request(statusKey(srv), srv.address, srv.port, true);
        gPage = Page::List;
        return;
    }
    if (gFields.empty() || gFormActive < 0 || gFormActive >= static_cast<int>(gFields.size())) {
        return;
    }
    auto* value = gFields[static_cast<std::size_t>(gFormActive)].value;
    if (!value) {
        return;
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl && keyEdge(0x56)) {
        auto text = clipboardText();
        if (!text.empty()) {
            *value += text;
        }
        return;
    }
    if (keyEdge(VK_BACK)) {
        while (!value->empty() && (static_cast<unsigned char>(value->back()) & 0xC0) == 0x80) {
            value->pop_back();
        }
        if (!value->empty()) {
            value->pop_back();
        }
        return;
    }
    for (int vk = 0x30; vk <= 0x39; ++vk) {
        if (keyEdge(vk)) {
            *value += static_cast<char>(vk);
        }
    }
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) {
        if (keyEdge(vk)) {
            *value += static_cast<char>('0' + (vk - VK_NUMPAD0));
        }
    }
    if (keyEdge(VK_DECIMAL)) {
        *value += '.';
    }
    for (int vk = 0x41; vk <= 0x5A; ++vk) {
        if (keyEdge(vk)) {
            char c = vkToChar(vk);
            if (c) {
                *value += c;
            }
        }
    }
    int const punct[] = {VK_OEM_PERIOD, VK_OEM_COMMA, VK_OEM_MINUS, VK_OEM_PLUS, VK_OEM_1, VK_OEM_2,
                         VK_OEM_3,      VK_OEM_4,     VK_OEM_5,     VK_OEM_6,    VK_OEM_7, VK_SPACE};
    for (int vk : punct) {
        if (keyEdge(vk)) {
            char c = vkToChar(vk);
            if (c) {
                *value += c;
            }
        }
    }
}

bool queryCursorClient(int& outX, int& outY);

void drawProbe(ll::event::render::AfterUIRenderEvent& event) {
    auto& ctx = event.uiRenderContext();
    auto& view = event.screenView();
    // AfterUIRenderEvent fires for EACH ScreenView, not once per frame. Drawing
    // again on toast/debug was the source of duplicated dark overlay layers.
    if (view.getScreenName() != "start_screen") return;
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
        probeLogger().info("[UI/native-v2] {} modal={} focusModal={} play=({},{} {}x{}) step={}",
            state, menu.modalVisible, menu.focusModal, menu.play.x0,menu.play.y0,
            menu.play.width(),menu.play.height(),menu.rowStep);
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
    if (!allowed) return;
    FontHandle handle = ci->getFontHandle();
    auto font=handle.mDefaultFont;
    if (!font) { gVisible.store(false); return; }
    // Keep the game's logical pixel size. Window resize/GUI-scale changes are
    // already reflected in the Play control geometry and the UI coordinate space.
    gU=1.0f;
    auto const entry = vanilla::menuEntry(menu.play,menu.rowStep);
    int px=0, py=0;
    if (queryCursorClient(px,py)) {
        float const scale=ci->getGuiScale();
        if (scale>0.0f) { std::lock_guard lock(gMutex); gMouseX=px/scale; gMouseY=py/scale; gMouseSeen=true; }
    }

    gTextKeep.clear();

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

    if (!open) {
        drawMenuButton(ctx, *font, entry, mx, my, mouseSeen);
    } else if (page == Page::List) {
        drawListPage(ctx, *font, sw, sh, mx, my, mouseSeen);
    } else if (page == Page::Form) {
        drawFormPage(ctx, *font, sw, sh, mx, my, mouseSeen);
    } else {
        drawConfirmPage(ctx, *font, sw, sh, mx, my, mouseSeen);
    }

    drawToast(ctx, *font, sw, sh);
    ctx.flushText(0.0f, std::nullopt);
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

// 解析 "host:port" / "[::1]:port" / "host"，缺省 19132
void parseAddress(std::string const& raw, std::string& host, int& port) {
    host = raw;
    port = 19132;
    // 去掉空白
    host.erase(std::remove_if(host.begin(), host.end(), [](unsigned char c) { return std::isspace(c) != 0; }), host.end());
    if (host.empty()) {
        return;
    }
    std::size_t colon = host.rfind(':');
    if (host.front() == '[') {
        auto close = host.find(']');
        if (close != std::string::npos && colon != std::string::npos && colon > close) {
            host = host.substr(1, close - 1) + host.substr(close + 1);
            colon = host.rfind(':');
        }
    }
    if (colon != std::string::npos) {
        auto portText = host.substr(colon + 1);
        bool numeric  = !portText.empty() && portText.size() <= 5
                     && std::all_of(portText.begin(), portText.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (numeric) {
            int value = std::stoi(portText);
            if (value > 0 && value <= 65535) {
                port = value;
            }
            host = host.substr(0, colon);
        }
    }
}

void startAddForm() {
    std::lock_guard lock(gMutex);
    gDraft       = FormDraft{};
    gFormId      = {};
    gFormActive  = 0;
    gFormEditing = false;
    rebuildFields();
    gPage = Page::Form;
    probeLogger().info("[UI] form opened (add)");
}

SavedServer draftToServer() {
    SavedServer srv;
    if(auto existing=ServerStore::getInstance().get(gFormId))srv=*existing;
    auto const previous=statusKey(srv);
    srv.id   = gFormId;
    srv.name = gDraft.name;
    parseAddress(gDraft.address, srv.address, srv.port);
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
    if (rawAction != 0 || sMouseLogs.load() < 5) {
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
    Rect  addBtn{}, refreshBtn{}, closeBtn{}, prevBtn{}, nextBtn{}, saveBtn{}, cancelBtn{}, yesBtn{}, noBtn{};
    Rect  addHit{}, refreshHit{}, closeHit{};
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
        addHit      = gAddHit;
        refreshHit  = gRefreshHit;
        closeHit    = gCloseHit;
        prevBtn     = gPrevBtn;
        nextBtn     = gNextBtn;
        saveBtn     = gSaveBtn;
        cancelBtn   = gCancelBtn;
        yesBtn      = gYesBtn;
        noBtn       = gNoBtn;
        fields      = gFields;
        formId      = gFormId;
    }

    probeLogger().info("[UI] click ui=({}, {}) page={} open={}", uiX, uiY, static_cast<int>(page), open ? 1 : 0);

    if (!open) {
        if (buttonValid && button.contains(uiX, uiY)) {
            event.cancel();
            refreshServers();
            std::lock_guard lock(gMutex);
            gOpen = true;
            gPage = Page::List;
        }
        return;
    }

    if (page == Page::List) {
        if (closeBtn.contains(uiX, uiY)) {
            probeLogger().info("[UI] hit close");
            std::lock_guard lock(gMutex);
            gOpen = false;
            return;
        }
        if (addBtn.contains(uiX, uiY)) {
            probeLogger().info("[UI] hit add");
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
            return;
        }
        if (nextBtn.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gScrollRow += 1;
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
                gDraft.address   = srv->address + ":" + std::to_string(srv->port);
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
            gPage = Page::List;
            return;
        }
        return;
    }

    // Form
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].rect.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gFormActive = static_cast<int>(i);
            return;
        }
    }
    if (saveBtn.contains(uiX, uiY)) {
        SavedServer srv;
        {
            std::lock_guard lock(gMutex);
            srv = draftToServer();
        }
        if (srv.address.empty()) {
            respondError("地址不能为空");
            return;
        }
        auto id = ServerStore::getInstance().upsert(srv);

        ServerStore::getInstance().save();
        ServerStatus::getInstance().request(statusKey(srv), srv.address, srv.port, true);
        {
            std::lock_guard lock(gMutex);
            gFormId = id;
            gPage   = Page::List;
        }
        return;
    }
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

}

void unregisterProbeOverlay() {
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
}

} // namespace sle::client