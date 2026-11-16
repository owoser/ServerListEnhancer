#include "ServerListPage.h"

#include "OreUIBridge.h"
#include "ServerStore.h"

#include "api/IDearOreUIApi.h"
#include "api/IPageApi.h"
#include "api/manifest/ModManifest.h"
#include "api/manifest/UiManifest.h"

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace sle::client {

using namespace dearoreui::api;

namespace {

constexpr char kNamespace[]   = "serverlistenhancer";
constexpr char kPanelId[]     = "server_list_panel";
constexpr char kFingerprint[] = "sle.panel.4";

// 与 ProbeOverlay 手绘几何保持一致（UI 单位 / 565 高、933 宽）
constexpr int kVisibleRows = 4;

// 原版 OreUI 主题字体（menus-theme 的 --fontsUi / --fontsBody）
constexpr char kFont[] = "font-family:'Minecraft Seven v2','Noto Sans',sans-serif;";

struct State {
    IDearOreUIApi*     api{};
    ModId              owner;
    RegistrationHandle overlay;
    SubscriptionHandle pageSub;
    bool               modRegistered{};
    bool               mounted{};
    bool               confirmed{};   // 确认面板真的被挂载到页面上
    std::uint64_t      floor{};       // 注册时已存在的最大 context id
    int                scroll{};
};

State& state() {
    static State s;
    return s;
}

std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

std::string firstGlyph(std::string const& text) {
    if (text.empty()) {
        return "?";
    }
    std::size_t len = 1;
    auto        c   = static_cast<unsigned char>(text[0]);
    if (c >= 0xF0) {
        len = 4;
    } else if (c >= 0xE0) {
        len = 3;
    } else if (c >= 0xC0) {
        len = 2;
    }
    return text.substr(0, std::min(len, text.size()));
}

void paletteOf(std::string const& id, int& r, int& g, int& b) {
    static int const pal[6][3] = {
        {78, 106, 140},
        {110, 92, 140},
        {92, 132, 104},
        {140, 106, 84},
        {96, 120, 140},
        {130, 96, 112},
    };
    int idx = static_cast<int>(std::hash<std::string>{}(id) % 6);
    r       = pal[idx][0];
    g       = pal[idx][1];
    b       = pal[idx][2];
}

std::string buildRowHtml(SavedServer const& server) {
    int r = 0, g = 0, b = 0;
    paletteOf(server.id, r, g, b);

    std::string title = server.name.empty() ? server.address : server.name;
    std::string sub   = server.address + ":" + std::to_string(server.port);
    std::string intro = server.introText;
    if (intro.empty() && !server.statusName.empty()) {
        intro = server.statusName;
    }

    std::string players   = "--";
    std::string playerCol = "rgba(255,255,255,0.42)";
    if (server.maxPlayers > 0) {
        players   = std::to_string(server.players) + "/" + std::to_string(server.maxPlayers);
        playerCol = "#C4C4CC";
    } else if (!server.statusName.empty()) {
        players = "离线";
    }

    std::string html;
    html += R"(<div style="position:relative;height:11.33vh;margin-bottom:1.06vh;box-sizing:border-box;background:rgba(36,36,41,0.99);border-top:0.09vh solid rgba(90,90,98,0.85);">)";
    html += std::string{R"(<div style=")"} + kFont + R"(position:absolute;left:10px;top:8px;width:48px;height:48px;display:flex;align-items:center;justify-content:center;font-size:2.6vh;color:#f0f0f0;background:rgb()" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + R"();">)" + escapeHtml(firstGlyph(title)) + "</div>";
    html += R"(<div style="position:absolute;left:68px;right:130px;top:6px;">)";
    html += std::string{R"(<div style=")"} + kFont + R"(font-size:2.05vh;color:#ffffff;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;">)" + escapeHtml(title) + "</div>";
    if (!intro.empty()) {
        html += std::string{R"(<div style=")"} + kFont + R"(font-size:1.5vh;color:#b1b2b5;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px;">)" + escapeHtml(intro) + "</div>";
    }
    html += std::string{R"(<div style=")"} + kFont + R"(font-size:1.4vh;color:#8c8c96;margin-top:2px;">)" + escapeHtml(sub) + "</div>";
    html += "</div>";
    html += std::string{R"(<div style=")"} + kFont + R"(position:absolute;right:8px;top:8px;font-size:1.5vh;color:)" + playerCol + R"(;">)" + escapeHtml(players) + "</div>";
    html += "</div>";
    return html;
}

std::string buildPanelHtml() {
    auto&       store   = ServerStore::getInstance();
    auto        servers = store.list();
    std::size_t total   = servers.size();
    int         first   = state().scroll;

    std::string html;
    html += R"(<div style="position:absolute;left:18%;top:10%;width:64%;height:82%;box-sizing:border-box;background:rgba(28,28,32,0.97);border:0.18vh solid rgba(92,92,100,0.95);">)";

    // 标题栏
    html += R"(<div style="position:absolute;left:0;top:0;right:0;height:42px;box-sizing:border-box;background:rgba(38,38,43,1);border-bottom:0.09vh solid rgba(88,88,96,0.9);display:flex;align-items:center;padding:0 16px;">)";
    html += std::string{R"(<div style=")"} + kFont + R"(font-size:2.4vh;color:#ffffff;">服务器列表</div>)";
    html += std::string{R"(<div style=")"} + kFont + R"(margin-left:auto;font-size:1.4vh;color:rgba(255,255,255,0.42);">)" + std::to_string(total) + " 个服务器</div>";
    html += "</div>";

    // 行区域（与手绘 rowsTop=panel.y0+52 / rowH=64 / gap=6 对应）
    html += R"(<div style="position:absolute;left:12px;right:12px;top:52px;bottom:66px;overflow:hidden;">)";
    if (servers.empty()) {
        html += std::string{R"(<div style=")"} + kFont + R"(font-size:1.6vh;color:rgba(255,255,255,0.55);text-align:center;padding-top:40px;">还没有服务器，点「添加服务器」新建一条</div>)";
    } else {
        int shown = 0;
        for (std::size_t i = static_cast<std::size_t>(std::max(0, first)); i < servers.size() && shown < kVisibleRows; ++i, ++shown) {
            html += buildRowHtml(servers[i]);
        }
    }
    html += "</div>";
    html += "</div>";
    return html;
}

void logError(std::string_view what, Error const& error) {
    ll::mod::NativeMod::current()->getLogger().error(
        "{} 失败: code={}, message={}",
        what,
        static_cast<int>(error.code),
        error.message
    );
}

UiManifest makeManifest() {
    UiManifest ui;
    ui.modNamespace  = kNamespace;
    ui.id            = kPanelId;
    ui.kind          = UiKind::Panel;
    ui.pageScopes    = {PageScope::MainMenu};
    ui.anchor        = UiAnchor::FullScreen;
    ui.pointerEvents = false;
    ui.fingerprint   = kFingerprint;
    ui.permissions.grant(Permission::UiMount);
    return ui;
}

// 把面板注册进 DearOreUI（每次页面创建前都要注册，planner 才会把它排进挂载计划）
bool registerPanelNow() {
    auto& s = state();
    if (s.api == nullptr || !s.modRegistered) {
        return false;
    }
    if (s.mounted) {
        static_cast<void>(s.api->unregisterUi(s.overlay));
        s.mounted = false;
    }
    auto result = s.api->registerPanel(s.owner, makeManifest(), buildPanelHtml());
    if (result.isErr()) {
        logError("registerPanel", result.error());
        return false;
    }
    s.overlay = result.value();
    s.mounted = true;

    // 页面上下文可能早已创建（DearOreUI 只在页面创建时挂载），注册后主动补挂一次
    int const remounted = OreUIBridge::getInstance().remountAll();
    if (remounted >= 0) {
        ll::mod::NativeMod::current()->getLogger().info("已请求 DearOreUI 补挂，处理上下文数={}", remounted);
    }
    return true;
}

} // namespace

bool installServerListPage() {
    auto& logger = ll::mod::NativeMod::current()->getLogger();

    auto& bridge = OreUIBridge::getInstance();
    if (!bridge.attach()) {
        return false;
    }

    auto& s = state();
    if (s.modRegistered) {
        return true;
    }

    s.api = static_cast<IDearOreUIApi*>(bridge.api());
    if (s.api == nullptr || !s.api->isReady()) {
        logger.warn("DearOreUI API 未就绪，跳过服务器列表页面注册");
        return false;
    }

    ModManifest manifest;
    manifest.id           = ModId{std::string{kNamespace}};
    manifest.modNamespace = kNamespace;
    manifest.displayName  = "Server List Enhancer";
    manifest.modVersion   = Version{1, 0, 0};
    manifest.permissions  = {Permission::PageObserve, Permission::UiMount, Permission::HostReadOnly};

    auto modResult = s.api->registerMod(manifest);
    if (modResult.isErr()) {
        logError("registerMod", modResult.error());
        return false;
    }
    s.owner         = modResult.value();
    s.modRegistered = true;

    // 关键：DearOreUI 只在页面 ready 时按已有注册生成挂载计划，
    // 所以必须在 PageEvent::Created 回调里（planner 运行之前）完成注册。
    PageSubscriptionOptions options;
    options.owner  = s.owner;
    options.scopes = {PageScope::MainMenu, PageScope::Any};
    auto subResult = s.api->subscribePage(
        options,
        PageEvent::Created,
        [](PageContextView const& page) {
            auto& st = state();
            if (st.api == nullptr) {
                return;
            }
            ll::mod::NativeMod::current()->getLogger().info(
                "页面创建 (scope={})，重新注册服务器列表面板",
                static_cast<int>(page.page.scope)
            );
            st.floor = page.id.value() > 0 ? page.id.value() - 1 : 0;
            registerPanelNow();
        }
    );
    if (subResult.isErr()) {
        logError("subscribePage", subResult.error());
    } else {
        s.pageSub = subResult.value();
    }

    // 只有收到「注册之后新创建的页面」的 Ready，才能确定面板确实进了挂载计划
    PageSubscriptionOptions readyOptions;
    readyOptions.owner  = s.owner;
    readyOptions.scopes = {PageScope::MainMenu, PageScope::Any};
    auto readyResult    = s.api->subscribePage(
        readyOptions,
        PageEvent::Ready,
        [](PageContextView const& page) {
            auto& st = state();
            if (page.id.value() > st.floor) {
                if (!st.confirmed) {
                    ll::mod::NativeMod::current()->getLogger().info(
                        "服务器列表面板已挂载 (context={})",
                        page.id.value()
                    );
                }
                st.confirmed = true;
            }
        }
    );
    if (readyResult.isErr()) {
        logError("subscribePage(Ready)", readyResult.error());
    }

    if (!registerPanelNow()) {
        return false;
    }
    logger.info("服务器列表面板已注册 (handle={})", s.overlay.value());
    return true;
}

void uninstallServerListPage() {
    auto& s = state();
    if (s.api != nullptr && s.pageSub.value() != 0) {
        static_cast<void>(s.api->unsubscribePage(s.pageSub));
        s.pageSub = SubscriptionHandle{};
    }
    if (s.mounted && s.api != nullptr) {
        static_cast<void>(s.api->unregisterUi(s.overlay));
    }
    s.mounted = false;
    s.overlay = RegistrationHandle{};
}

void refreshServerListPage() {
    static_cast<void>(registerPanelNow());
}

void setServerListScroll(int firstIndex) {
    auto& s = state();
    if (s.scroll == firstIndex) {
        return;
    }
    s.scroll = firstIndex;
    refreshServerListPage();
}

bool htmlListActive() {
    // 注：注入脚本 rc=0 但页面看不见 DOM，先保持手绘层渲染，
    // 等定位到可见性问题（容器/层叠）后再打开接管。
    return false;
}

namespace {

std::string escapeJs(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char ch : text) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(ch);
        }
    }
    return out;
}

std::string buildMountScript() {
    std::string js;
    js += "\n(function(){try{";
    js += "var id='sle-list-root';var el=document.getElementById(id);";
    js += "var host=document.getElementById('root')||document.body;";
    js += "if(!el){el=document.createElement('div');el.id=id;";
    js += "el.style.cssText='position:fixed;left:0;top:0;right:0;bottom:0;z-index:2147483647;pointer-events:none;';";
    js += "if(host){host.appendChild(el);}else{return;}}";
    js += "var probe=document.getElementById('sle-probe');";
    js += "if(!probe){probe=document.createElement('div');probe.id='sle-probe';";
    js += "probe.style.cssText='position:absolute;left:4px;top:4px;width:72px;height:22px;background:#ff0000;color:#ffffff;font-size:14px;z-index:2147483647;';";
    js += "probe.textContent='SLE-OK';el.appendChild(probe);}";
    js += "var panel=document.getElementById('sle-panel');";
    js += "if(!panel){panel=document.createElement('div');panel.id='sle-panel';el.appendChild(panel);}";
    js += "panel.innerHTML=\"" + escapeJs(buildPanelHtml()) + "\";";
    js += "window.__sleMounted=(window.__sleMounted||0)+1;";
    js += "}catch(e){try{window.__sleError=String(e&&e.message);}catch(_){}}})();\n";
    return js;
}
} // namespace

void pushListToPage() {
    auto& s = state();
    if (!s.modRegistered || s.api == nullptr) {
        return;
    }
    int const rc = OreUIBridge::getInstance().executeScript(buildMountScript());
    static int lastRc = 999;
    if (rc != lastRc) {
        lastRc = rc;
        ll::mod::NativeMod::current()->getLogger().info("注入列表脚本 rc={}", rc);
    }
}

void pumpServerListPage() {
    static bool installed = false;
    static int  attempts  = 0;
    if (installed) {
        return;
    }
    if (++attempts > 1800) {
        installed = true;
        ll::mod::NativeMod::current()->getLogger().warn("等待 DearOreUI 超时，服务器列表页面未注册");
        return;
    }
    if (installServerListPage()) {
        installed = true;
    }
}

} // namespace sle::client
