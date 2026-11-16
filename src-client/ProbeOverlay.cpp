#include "ProbeOverlay.h"

#include "ClientEntry.h"
#include "VisualCache.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/FontHandle.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/screens/interfaces/ISceneStack.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/input/RectangleArea.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

// 原版菜单按钮实测尺寸：288x44 px @ guiScale 2.0 = 144x22 UI
constexpr float kButtonW = 144.0f;
constexpr float kButtonH = 26.0f;

// 渲染颜色是 linear 空间，输出会做 sRGB 编码，所以按 sRGB 目标值反算
constexpr float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

mce::Color color8(int r, int g, int b, float alpha = 1.0f) {
    // 实测：传入值按 0..1 直通成 0..255（不做 sRGB 编码），所以直接给分量
    return mce::Color(r / 255.0f, g / 255.0f, b / 255.0f, alpha);
}

std::mutex        gStateMutex;
Rect              gButton{};
bool              gButtonValid{};
Rect              gCloseButton{};
bool              gCloseValid{};
std::vector<Rect> gRowRects{};
std::vector<std::string> gRowKeys{};
Rect              gPanel{};
bool              gPanelValid{};
float             gMouseX{};
float             gMouseY{};
bool              gMouseSeen{};

std::atomic<bool> gExpanded{false};
std::atomic<bool> gVisible{false};
std::atomic<int>  gDrawCount{0};
Clock::time_point gLastClick{};

thread_local bool tTracing = false;

ll::event::ListenerPtr gRenderListener{};
ll::event::ListenerPtr gMouseListener{};

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

// 原版风格按钮（按截图逐像素复刻）：1 UI 黑边 + #C6C6C6 底 + 顶部 #F7F7F7 + 底边 #656565
void drawVanillaButton(
    MinecraftUIRenderContext& ctx,
    Font&                     font,
    Rect const&               r,
    std::string_view          label,
    bool                      hot
) {
    Rect frame{r.x0 - 1.0f, r.y0 - 1.0f, r.x1 + 1.0f, r.y1 + 1.0f};
    fillRect(ctx, frame, color8(19, 19, 19), 1.0f);
    fillRect(ctx, r, color8(hot ? 214 : 198, hot ? 214 : 198, hot ? 214 : 198), 1.0f);

    Rect topLine{r.x0, r.y0, r.x1, r.y0 + 1.0f};
    fillRect(ctx, topLine, color8(247, 247, 247), 1.0f);
    Rect bottomLine{r.x0, r.y1 - 1.0f, r.x1, r.y1};
    fillRect(ctx, bottomLine, color8(101, 101, 101), 1.0f);

    // 角落 1 单位黑块，制造原版的轻微圆角观感
    fillRect(ctx, Rect{r.x0, r.y0, r.x0 + 1.0f, r.y0 + 1.0f}, color8(19, 19, 19), 1.0f);
    fillRect(ctx, Rect{r.x1 - 1.0f, r.y0, r.x1, r.y0 + 1.0f}, color8(19, 19, 19), 1.0f);
    fillRect(ctx, Rect{r.x0, r.y1 - 1.0f, r.x0 + 1.0f, r.y1}, color8(19, 19, 19), 1.0f);
    fillRect(ctx, Rect{r.x1 - 1.0f, r.y1 - 1.0f, r.x1, r.y1}, color8(19, 19, 19), 1.0f);

    Rect textRect{r.x0, r.y0 + 9.0f, r.x1, r.y1};
    drawText(ctx, font, textRect, label, color8(26, 26, 26), 0.95f, ui::TextAlignment::Center);
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

    static float lastTracedW = -1.0f;
    static float lastTracedH = -1.0f;
    int          frame       = gDrawCount.fetch_add(1);
    tTracing                 = frame < 3 || sw != lastTracedW || sh != lastTracedH;
    if (tTracing) {
        lastTracedW = sw;
        lastTracedH = sh;
        trace("A hook ui=" + std::to_string(sw) + "x" + std::to_string(sh));
    }

    auto optCi = ll::service::bedrock::getClientInstance();
    if (!optCi) {
        return;
    }

    FontHandle handle = optCi->getFontHandle();
    auto       font   = handle.mDefaultFont;
    if (!font) {
        return;
    }

    // 只在主菜单绘制：当前场景栈里的屏幕名全部是主菜单自己的那几个时才画；
    // 任何弹窗（退出确认等）会往栈里压新屏幕，名字不在白名单 -> 不画
    static std::array<std::string_view, 4> const kMainMenuScreens{"start_screen", "toast_screen", "debug_screen", "cubemap_background_screen"};

    static bool                     cachedAllowed = false;
    static int                      cacheTick     = 0;
    static std::string              lastLogged;
    if ((cacheTick++ % 4) == 0) {
        std::vector<std::string> names;
        if (auto ci = ll::service::bedrock::getClientInstance()) {
            auto stack = ci->getCurrentSceneStack();
            names      = stack->getScreenNames();
        }
        bool allowed = !names.empty();
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

    bool  expanded = gExpanded.load();
    float mx{}, my{};
    bool  mouseSeen{};
    {
        std::lock_guard lock(gStateMutex);
        mx        = gMouseX;
        my        = gMouseY;
        mouseSeen = gMouseSeen;
        gRowRects.clear();
        gRowKeys.clear();
        gCloseValid = false;
    }

    if (!expanded) {
        // ---- 收起态：菜单列顶部一个原版风格按钮 ----
        Rect button{};
        button.x0 = sw * 0.5f - kButtonW * 0.5f;
        button.y0 = sh * 0.4743f;
        button.x1 = button.x0 + kButtonW;
        button.y1 = button.y0 + kButtonH;

        bool hot = mouseSeen && button.contains(mx, my);
        {
            std::lock_guard lock(gStateMutex);
            gButton      = button;
            gButtonValid = true;
            gPanelValid  = false;
        }

        drawVanillaButton(ctx, *font, button, std::string_view{"服务器列表"}, hot);
        ctx.flushImages(mce::Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, HashedString("ui_fillColor"));
        ctx.flushText(0.0f, std::nullopt);
        if (tTracing) {
            trace("L text done (collapsed)");
        }
        return;
    }

    // ---- 展开态：遮罩 + 面板 + 行列表 + 关闭按钮（收起时完全不画顶部按钮，避免文字叠在面板上）----
    {
        std::lock_guard lock(gStateMutex);
        gButtonValid = false;
        gPanelValid  = true;
    }

    Rect scrim{0.0f, 0.0f, sw, sh};
    fillRect(ctx, scrim, color8(0, 0, 0), 0.45f);

    Rect panel{};
    panel.x0 = sw * 0.22f;
    panel.y0 = sh * 0.13f;
    panel.x1 = sw * 0.78f;
    panel.y1 = sh * 0.86f;
    {
        std::lock_guard lock(gStateMutex);
        gPanel = panel;
    }

    fillRect(ctx, panel, color8(28, 28, 32), 0.97f);
    Rect panelBorder{panel.x0 - 1.0f, panel.y0 - 1.0f, panel.x1 + 1.0f, panel.y1 + 1.0f};
    fillRect(ctx, panelBorder, color8(96, 96, 104), 0.95f);

    // 标题栏
    Rect header{panel.x0, panel.y0, panel.x1, panel.y0 + 42.0f};
    fillRect(ctx, header, color8(38, 38, 43), 1.0f);
    Rect headerLine{panel.x0, panel.y0 + 42.0f, panel.x1, panel.y0 + 43.0f};
    fillRect(ctx, headerLine, color8(88, 88, 96), 0.9f);

    // 关闭按钮
    Rect close{};
    close.x0 = sw * 0.5f - kButtonW * 0.5f;
    close.y0 = panel.y1 - 50.0f;
    close.x1 = close.x0 + kButtonW;
    close.y1 = close.y0 + kButtonH;
    bool closeHot = mouseSeen && close.contains(mx, my);
    drawVanillaButton(ctx, *font, close, std::string_view{"关闭"}, closeHot);
    {
        std::lock_guard lock(gStateMutex);
        gCloseButton = close;
        gCloseValid  = true;
    }

    // 行列表
    auto entries = VisualCache::getInstance().entries();
    std::sort(entries.begin(), entries.end(), [](auto const& l, auto const& r) { return l.first < r.first; });

    float rowX0  = panel.x0 + 14.0f;
    float rowX1  = panel.x1 - 14.0f;
    float rowY   = panel.y0 + 56.0f;
    float rowH   = 46.0f;
    float rowGap = 6.0f;
    float bottom = close.y0 - 12.0f;

    std::size_t shown = 0;
    {
        std::lock_guard lock(gStateMutex);
        for (auto const& [key, cached] : entries) {
            if (rowY + rowH > bottom) {
                break;
            }
            Rect row{rowX0, rowY, rowX1, rowY + rowH};
            bool hot = mouseSeen && row.contains(mx, my);
            fillRect(ctx, row, color8(hot ? 62 : 48, hot ? 62 : 48, hot ? 70 : 54), 0.96f);
            gRowRects.push_back(row);
            gRowKeys.push_back(key);
            rowY += rowH + rowGap;
            ++shown;
        }
    }

    ctx.flushImages(mce::Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, HashedString("ui_fillColor"));

    // ---- 文字 ----
    Rect titleRect{panel.x0 + 16.0f, panel.y0 + 10.0f, panel.x1 - 16.0f, panel.y0 + 38.0f};
    drawText(ctx, *font, titleRect, std::string_view{"服务器列表"}, color8(255, 255, 255), 1.5f);

    if (entries.empty()) {
        Rect emptyRect{panel.x0 + 16.0f, panel.y0 + 96.0f, panel.x1 - 16.0f, panel.y0 + 124.0f};
        drawText(
            ctx,
            *font,
            emptyRect,
            std::string_view{"还没有服务器档案，先进一次服务器试试"},
            color8(180, 180, 188),
            1.5f,
            ui::TextAlignment::Center
        );
    } else {
        for (std::size_t i = 0; i < shown && i < entries.size(); ++i) {
            auto const& [key, cached] = entries[i];
            std::string name         = key;
            if (auto pos = name.find("name:"); pos != std::string::npos) {
                name = name.substr(pos + 5);
            }

            Rect nameRect{gRowRects[i].x0 + 10.0f, gRowRects[i].y0 + 6.0f, gRowRects[i].x1 - 10.0f, gRowRects[i].y0 + 24.0f};
            drawText(ctx, *font, nameRect, std::string_view{name}, color8(255, 255, 255), 1.5f);

            std::string intro = cached.spec.introText;
            if (intro.empty()) {
                intro = cached.spec.logoId.empty() ? "（无档案）" : "logo=" + cached.spec.logoId;
            }
            Rect introRect{gRowRects[i].x0 + 10.0f, gRowRects[i].y0 + 26.0f, gRowRects[i].x1 - 10.0f, gRowRects[i].y1 - 4.0f};
            drawText(
                ctx,
                *font,
                introRect,
                std::string_view{truncateUtf8(intro, 140)},
                color8(176, 176, 184),
                1.0f
            );
        }
    }

    ctx.flushText(0.0f, std::nullopt);
    if (tTracing) {
        trace("L text done (expanded) rows=" + std::to_string(shown));
    }
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

    Rect              button{};
    bool              buttonValid{};
    Rect              close{};
    bool              closeValid{};
    Rect              panel{};
    std::vector<Rect> rows;
    std::vector<std::string> keys;
    bool              expanded = gExpanded.load();
    {
        std::lock_guard lock(gStateMutex);
        gMouseX     = uiX;
        gMouseY     = uiY;
        gMouseSeen  = true;
        button      = gButton;
        buttonValid = gButtonValid;
        close       = gCloseButton;
        closeValid  = gCloseValid;
        panel       = gPanel;
        rows        = gRowRects;
        keys        = gRowKeys;
    }

    int  buttonId = static_cast<int>(event.buttonData());
    int  actionId = static_cast<int>(event.actionButtonId());
    bool pressed  = buttonId != 0 || actionId != 0;
    if (!pressed) {
        return;
    }

    auto now = Clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - gLastClick).count() < 250) {
        return;
    }
    gLastClick = now;

    bool inButton = buttonValid && button.contains(uiX, uiY);
    bool inClose  = closeValid && close.contains(uiX, uiY);
    int  rowIndex = -1;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].contains(uiX, uiY)) {
            rowIndex = static_cast<int>(i);
            break;
        }
    }

    probeLogger().info(
        "[UI] 点击 ui=({}, {}) button={} close={} row={} action={}",
        uiX,
        uiY,
        inButton,
        inClose,
        rowIndex,
        actionId
    );

    if (!expanded && inButton) {
        gExpanded.store(true);
        probeLogger().info("[UI] 面板 展开");
        return;
    }
    if (expanded && inClose) {
        gExpanded.store(false);
        probeLogger().info("[UI] 面板 收起（关闭按钮）");
        return;
    }
    if (expanded && rowIndex >= 0 && rowIndex < static_cast<int>(keys.size())) {
        probeLogger().info("[UI] 点击服务器行 key={}", keys[rowIndex]);
        return;
    }
    if (expanded && !panel.contains(uiX, uiY)) {
        gExpanded.store(false);
        probeLogger().info("[UI] 面板 收起（点击面板外）");
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
}

} // namespace sle::client