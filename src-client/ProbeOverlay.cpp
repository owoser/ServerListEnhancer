#include "ProbeOverlay.h"

#include "ClientEntry.h"
#include "ServerStatus.h"
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

struct Rect {
    float x0{};
    float y0{};
    float x1{};
    float y1{};
    bool  contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

constexpr float kButtonW = 144.0f;
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
int   gSelected{-1};
int   gScrollRow{0};
bool  gButtonValid{};
Rect  gButton{};
Rect  gPanel{};
bool  gPanelValid{};

std::vector<RowHit> gRows;
Rect                gAddBtn{};
Rect                gRefreshBtn{};
Rect                gCloseBtn{};
Rect                gPrevBtn{};
Rect                gNextBtn{};
Rect                gSaveBtn{};
Rect                gCancelBtn{};
Rect                gYesBtn{};
Rect                gNoBtn{};
bool                gBtnsValid{};

struct FormDraft {
    std::string name;
    std::string address;
    std::string port{"19132"};
    std::string logoUrl;
    std::string introText;
};

FormDraft               gDraft;
std::vector<FormField>  gFields;
std::string             gFormId;
int                     gFormActive{0};
bool                    gFormEditing{false};

std::string gConfirmText;
int         gConfirmId{-1};

std::string       gToast;
Clock::time_point gToastAt{};

float gMouseX{};
float gMouseY{};
bool  gMouseSeen{};

std::atomic<bool> gVisible{false};
std::atomic<int>  gDrawCount{0};
Clock::time_point gLastClick{};

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

void drawText(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    Rect const&               r,
    std::string_view          text,
    mce::Color const&         color,
    float                     fontSize,
    ui::TextAlignment         alignment = ui::TextAlignment::Left
) {
    RectangleArea area{};
    area._x0 = r.x0;
    area._x1 = r.x1;
    area._y0 = r.y0;
    area._y1 = r.y1;

    TextMeasureData  textData{fontSize, 0.0f, true, false, false, alignment};
    CaretMeasureData caretData{0, false};
    ctx.drawText(font, area, text, color, 1.0f, alignment, textData, caretData);
}

mce::Color color8(int r, int g, int b, float alpha = 1.0f) {
    // 管线会把 linear 值编码成 sRGB 输出，所以这里先做 sRGB -> linear 反算
    auto toLinear = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return mce::Color(toLinear(r / 255.0f), toLinear(g / 255.0f), toLinear(b / 255.0f), alpha);
}

void drawVanillaButton(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    Rect const&               r,
    std::string_view          label,
    bool                      hot,
    float                     fontSize = 0.95f
) {
    Rect frame{r.x0 - 1.0f, r.y0 - 1.0f, r.x1 + 1.0f, r.y1 + 1.0f};
    fillRect(ctx, frame, color8(19, 19, 19), 1.0f);
    fillRect(ctx, r, color8(hot ? 214 : 198, hot ? 214 : 198, hot ? 214 : 198), 1.0f);
    fillRect(ctx, Rect{r.x0, r.y0, r.x1, r.y0 + 1.0f}, color8(247, 247, 247), 1.0f);
    fillRect(ctx, Rect{r.x0, r.y1 - 1.0f, r.x1, r.y1}, color8(101, 101, 101), 1.0f);
    fillRect(ctx, Rect{r.x0, r.y0, r.x0 + 1.0f, r.y0 + 1.0f}, color8(19, 19, 19), 1.0f);
    fillRect(ctx, Rect{r.x1 - 1.0f, r.y0, r.x1, r.y0 + 1.0f}, color8(19, 19, 19), 1.0f);
    fillRect(ctx, Rect{r.x0, r.y1 - 1.0f, r.x0 + 1.0f, r.y1}, color8(19, 19, 19), 1.0f);
    fillRect(ctx, Rect{r.x1 - 1.0f, r.y1 - 1.0f, r.x1, r.y1}, color8(19, 19, 19), 1.0f);

    Rect textRect{r.x0, r.y0 + 9.0f, r.x1, r.y1};
    drawText(ctx, font, textRect, label, color8(26, 26, 26), fontSize, ui::TextAlignment::Center);
}

std::string statusKey(SavedServer const& s) { return s.address + ":" + std::to_string(s.port); }

// 行内小按钮（编辑 / 删除）
bool drawSmallButton(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    Rect const&               r,
    std::string_view          label,
    float                     mx,
    float                     my,
    bool                      mouseSeen
) {
    bool hot = mouseSeen && r.contains(mx, my);
    fillRect(ctx, r, color8(hot ? 190 : 168, hot ? 190 : 168, hot ? 190 : 168), 1.0f);
    fillRect(ctx, Rect{r.x0, r.y0, r.x1, r.y0 + 1.0f}, color8(216, 216, 216), 1.0f);
    fillRect(ctx, Rect{r.x0, r.y1 - 1.0f, r.x1, r.y1}, color8(96, 96, 96), 1.0f);
    Rect textRect{r.x0, r.y0 + 4.0f, r.x1, r.y1};
    drawText(ctx, font, textRect, label, color8(30, 30, 30), 0.8f, ui::TextAlignment::Center);
    return hot;
}

void openForm(std::optional<SavedServer> const& existing) {
    std::lock_guard lock(gMutex);
    gFormId     = existing ? existing->id : std::string{};
    gFormActive = 0;
    gFormEditing = true;
    gPage       = Page::Form;
    gSelected   = -1;
}

} // namespace

// ------------------------------------------------------------------
// 渲染
// ------------------------------------------------------------------
namespace {

void drawMenuButton(MinecraftUIRenderContext& ctx, Font& font, float sw, float sh, float mx, float my, bool mouseSeen) {
    Rect button{};
    button.x0 = sw * 0.5f - kButtonW * 0.5f;
    button.y0 = sh * 0.4743f;
    button.x1 = button.x0 + kButtonW;
    button.y1 = button.y0 + kButtonH;

    {
        std::lock_guard lock(gMutex);
        gButton      = button;
        gButtonValid = true;
    }
    bool hot = mouseSeen && button.contains(mx, my);
    drawVanillaButton(ctx, font, button, std::string_view{"服务器列表"}, hot);
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
    Rect panel{sw * 0.18f, sh * 0.10f, sw * 0.82f, sh * 0.92f};
    fillRect(ctx, panel, color8(28, 28, 32), 0.97f);
    Rect border{panel.x0 - 1.0f, panel.y0 - 1.0f, panel.x1 + 1.0f, panel.y1 + 1.0f};
    fillRect(ctx, border, color8(92, 92, 100), 1.0f);

    Rect header{panel.x0, panel.y0, panel.x1, panel.y0 + 42.0f};
    fillRect(ctx, header, color8(38, 38, 43), 1.0f);
    fillRect(ctx, Rect{panel.x0, panel.y0 + 42.0f, panel.x1, panel.y0 + 43.0f}, color8(88, 88, 96), 0.9f);
    drawText(
        ctx,
        font,
        Rect{panel.x0 + 16.0f, panel.y0 + 10.0f, panel.x1 - 16.0f, panel.y0 + 38.0f},
        std::string_view{"服务器列表"},
        color8(255, 255, 255),
        1.5f
    );

    // 数据
    auto entries = ServerStore::getInstance().list();
    auto rowsTop    = panel.y0 + 52.0f;
    auto rowsBottom = panel.y1 - 66.0f;
    float rowH      = 64.0f;
    float rowGap    = 6.0f;
    float rowX0     = panel.x0 + 12.0f;
    float rowX1     = panel.x1 - 12.0f;

    int visible = std::max(1, static_cast<int>((rowsBottom - rowsTop) / (rowH + rowGap)));
    int maxScroll = std::max(0, static_cast<int>(entries.size()) - visible);

    {
        std::lock_guard lock(gMutex);
        gScrollRow = std::clamp(gScrollRow, 0, maxScroll);
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
            bool        isSel   = (idx == gSelected);
            RowHit      hit{};
            hit.row = row;
            hit.id  = e.id;
            hit.edit = Rect{rowX1 - 108.0f, y + rowH - 24.0f, rowX1 - 60.0f, y + rowH - 4.0f};
            hit.del  = Rect{rowX1 - 54.0f, y + rowH - 24.0f, rowX1 - 6.0f, y + rowH - 4.0f};
            gRows.push_back(hit);

            fillRect(
                ctx,
                row,
                isSel ? color8(58, 58, 66) : (hot ? color8(46, 46, 52) : color8(36, 36, 41)),
                1.0f
            );
            fillRect(ctx, Rect{row.x0, row.y0, row.x1, row.y0 + 1.0f}, color8(90, 90, 98), 0.8f);

            // Logo 占位（图片管线下一阶段接入）
            Rect logo{row.x0 + 10.0f, y + 8.0f, row.x0 + 58.0f, y + 56.0f};
            int  hue = static_cast<int>(std::hash<std::string>{}(e.id) % 6);
            static int const pal[6][3] = {
                {78, 106, 140},
                {110, 92, 140},
                {92, 132, 104},
                {140, 106, 84},
                {96, 120, 140},
                {130, 96, 112},
            };
            fillRect(ctx, logo, color8(pal[hue][0], pal[hue][1], pal[hue][2]), 1.0f);
            std::string initial = e.name.empty() ? "?" : truncateUtf8(e.name, 1);
            drawText(
                ctx,
                font,
                Rect{logo.x0, logo.y0 + 14.0f, logo.x1, logo.y1},
                std::string_view{initial},
                color8(240, 240, 240),
                2.0f,
                ui::TextAlignment::Center
            );

            // 名称 / 简介 / 地址
            auto        status = ServerStatus::getInstance().get(statusKey(e));
            std::string name   = e.name;
            if (name.empty() && status && status->ok) {
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

            std::string intro = e.introText;
            if (intro.empty() && status && status->ok) {
                intro = status->level;
            }
            if (!intro.empty()) {
                drawText(
                    ctx,
                    font,
                    Rect{row.x0 + 68.0f, y + 26.0f, row.x1 - 120.0f, y + 40.0f},
                    std::string_view{truncateUtf8(intro, 90)},
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
                if (status->ok) {
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
        if (entries.empty()) {
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
        float by = panel.y1 - 46.0f;
        gRefreshBtn = Rect{panel.x0 + 16.0f, by, panel.x0 + 128.0f, by + kButtonH};
        gAddBtn     = Rect{panel.x0 + 136.0f, by, panel.x0 + 248.0f, by + kButtonH};
        gCloseBtn   = Rect{panel.x1 - 144.0f, by, panel.x1 - 16.0f, by + kButtonH};

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
            color8(180, 180, 190),
            0.9f
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

    float by = panel.y1 - 44.0f;
    gSaveBtn   = Rect{panel.x1 - 300.0f, by, panel.x1 - 160.0f, by + kButtonH};
    gCancelBtn = Rect{panel.x1 - 148.0f, by, panel.x1 - 14.0f, by + kButtonH};
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
    gYesBtn      = Rect{box.x0 + 20.0f, by, box.x0 + 20.0f + 120.0f, by + kButtonH};
    gNoBtn       = Rect{box.x1 - 140.0f, by, box.x1 - 20.0f, by + kButtonH};
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

void drawProbe(ll::event::render::AfterUIRenderEvent& event) {
    auto& ctx  = event.uiRenderContext();
    auto& view = event.screenView();

    glm::vec2 size = view.mSize;
    float     sw   = size.x;
    float     sh   = size.y;
    if (!(sw > 1.0f) || !(sh > 1.0f)) {
        sw = 1920.0f;
        sh = 1080.0f;
    }

    static int frame = 0;
    ++frame;
    tTracing        = (frame < 3);

    auto optCi = ll::service::bedrock::getClientInstance();
    if (!optCi) {
        return;
    }
    FontHandle handle = optCi->getFontHandle();
    auto       font   = handle.mDefaultFont;
    if (!font) {
        return;
    }

    static std::array<std::string_view, 4> const kMainMenuScreens{
        "start_screen",
        "toast_screen",
        "debug_screen",
        "cubemap_background_screen"
    };
    static bool        cachedAllowed = false;
    static int         cacheTick     = 0;
    static std::string lastLogged;
    if ((cacheTick++ % 4) == 0) {
        std::vector<std::string> names;
        if (auto ci = ll::service::bedrock::getClientInstance()) {
            auto stack = ci->getCurrentSceneStack();
            names      = stack->getScreenNames();
        }
        bool        allowed = !names.empty();
        std::string joined;
        for (auto const& n : names) {
            if (!joined.empty()) {
                joined += "|";
            }
            joined += n;
            bool known = false;
            for (auto const& k : kMainMenuScreens) {
                if (n == k) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                allowed = false;
            }
        }
        if (joined != lastLogged) {
            lastLogged = joined;
            probeLogger().info("[UI] 场景栈: {} 绘制={}", joined, allowed);
        }
        cachedAllowed = allowed;
    }
    gVisible.store(cachedAllowed);
    if (!cachedAllowed) {
        return;
    }

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

    if (!open) {
        drawMenuButton(ctx, *font, sw, sh, mx, my, mouseSeen);
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
    gFields.push_back({"名称", &gDraft.name, {}, false});
    gFields.push_back({"地址", &gDraft.address, {}, false});
    gFields.push_back({"端口", &gDraft.port, {}, true});
    gFields.push_back({"Logo", &gDraft.logoUrl, {}, false});
    gFields.push_back({"简介", &gDraft.introText, {}, false});
}

void startAddForm() {
    std::lock_guard lock(gMutex);
    gDraft       = FormDraft{};
    gFormId      = {};
    gFormActive  = 0;
    gFormEditing = false;
    rebuildFields();
    gPage = Page::Form;
}

SavedServer draftToServer() {
    SavedServer srv;
    srv.id        = gFormId;
    srv.name      = gDraft.name;
    srv.address   = gDraft.address;
    srv.logoUrl   = gDraft.logoUrl;
    srv.introText = gDraft.introText;
    int port      = 0;
    for (char c : gDraft.port) {
        if (c < '0' || c > '9') {
            port = 0;
            break;
        }
        port = port * 10 + (c - '0');
        if (port > 65535) {
            break;
        }
    }
    srv.port = (port > 0 && port <= 65535) ? port : 19132;
    return srv;
}

void onMouse(ll::event::input::MouseInputEvent& event) {
    if (!gVisible.load()) {
        return;
    }
    auto ci = ll::service::bedrock::getClientInstance();
    if (!ci) {
        return;
    }
    float guiScale = ci->getGuiScale();
    if (!(guiScale > 0.01f)) {
        guiScale = 1.0f;
    }
    float uiX = static_cast<float>(event.x()) / guiScale;
    float uiY = static_cast<float>(event.y()) / guiScale;

    bool openNow{};
    {
        std::lock_guard lock(gMutex);
        gMouseX    = uiX;
        gMouseY    = uiY;
        gMouseSeen = true;
        openNow    = gOpen;
    }
    // 面板打开时拦截所有鼠标事件：否则点击会穿透到被遮住的原版按钮（游戏/设置）
    if (openNow) {
        event.cancel();
    }

    int  buttonId = static_cast<int>(event.buttonData());
    int  actionId = static_cast<int>(event.actionButtonId());
    if (buttonId == 0 && actionId == 0) {
        return;
    }
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
    std::vector<FormField> fields;
    std::string            formId;
    int                    formActive{};
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
        prevBtn     = gPrevBtn;
        nextBtn     = gNextBtn;
        saveBtn     = gSaveBtn;
        cancelBtn   = gCancelBtn;
        yesBtn      = gYesBtn;
        noBtn       = gNoBtn;
        fields      = gFields;
        formId      = gFormId;
        formActive  = gFormActive;
    }

    if (!open) {
        if (buttonValid && button.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gOpen = true;
            gPage = Page::List;
        }
        return;
    }

    if (page == Page::List) {
        if (closeBtn.contains(uiX, uiY)) {
            std::lock_guard lock(gMutex);
            gOpen = false;
            return;
        }
        if (addBtn.contains(uiX, uiY)) {
            startAddForm();
            return;
        }
        if (refreshBtn.contains(uiX, uiY)) {
            auto entries = ServerStore::getInstance().list();
            for (auto const& e : entries) {
                ServerStatus::getInstance().request(statusKey(e), e.address, e.port);
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
                gDraft.address   = srv->address;
                gDraft.port      = std::to_string(srv->port);
                gDraft.logoUrl   = srv->logoUrl;
                gDraft.introText = srv->introText;
                rebuildFields();
                gFormId      = srv->id;
                gFormActive  = 0;
                gFormEditing = true;
                gPage        = Page::Form;
                return;
            }
            if (row.row.contains(uiX, uiY)) {
                respondError("进服接口待 MCAPI 批准（#352）");
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
        ServerStatus::getInstance().request(statusKey(srv), srv.address, srv.port);
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

void onKey(ll::event::input::KeyInputEvent& event) {
    if (!gVisible.load() || !event.isDown()) {
        return;
    }
    bool open{};
    Page page{Page::List};
    {
        std::lock_guard lock(gMutex);
        open = gOpen;
        page = gPage;
    }
    if (!open) {
        return;
    }

    int code = event.keyCode();
    if (page == Page::List) {
        if (code == 27) { // Esc
            std::lock_guard lock(gMutex);
            gOpen = false;
        }
        return;
    }
    if (page == Page::Confirm) {
        if (code == 27) {
            std::lock_guard lock(gMutex);
            gPage = Page::List;
        }
        return;
    }

    // Form
    std::lock_guard lock(gMutex);
    if (code == 27) {
        gPage = Page::List;
        return;
    }
    if (code == 13) {
        auto srv = draftToServer();
        if (srv.address.empty()) {
            gToast   = "地址不能为空";
            gToastAt = Clock::now();
            return;
        }
        gFormId = ServerStore::getInstance().upsert(srv);
        ServerStore::getInstance().save();
        gPage = Page::List;
        return;
    }
    if (code == 9) { // Tab
        if (!gFields.empty()) {
            gFormActive = (gFormActive + 1) % static_cast<int>(gFields.size());
        }
        return;
    }
    if (!gFields.empty() && gFormActive >= 0 && gFormActive < static_cast<int>(gFields.size())) {
        auto* value = gFields[static_cast<std::size_t>(gFormActive)].value;
        if (!value) {
            return;
        }
        if (code == 8) { // Backspace
            if (!value->empty()) {
                value->pop_back();
            }
            return;
        }
        if (code >= 32 && code <= 126) {
            value->push_back(static_cast<char>(code));
        }
    }
}

} // namespace

void registerProbeOverlay() {
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
        gKeyListener = bus.emplaceListener<ll::event::input::KeyInputEvent>([](ll::event::input::KeyInputEvent& event) {
            onKey(event);
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