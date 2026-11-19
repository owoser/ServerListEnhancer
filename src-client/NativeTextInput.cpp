#include "NativeTextInput.h"

#include <imm.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace sle::client {
namespace {

constexpr wchar_t     kPopupClass[] = L"ServerListEnhancer.NativeTextInput";
constexpr wchar_t     kHostClass[]  = L"ServerListEnhancer.NativeTextInputHost";
constexpr UINT        kMsgOpen      = WM_APP + 41;
constexpr UINT        kMsgClose     = WM_APP + 42;
constexpr UINT        kMsgPosition  = WM_APP + 43;
constexpr UINT        kMsgCommit    = WM_APP + 44;
constexpr UINT        kMsgActivate  = WM_APP + 45;
constexpr int         kBorder       = 1;
constexpr int         kMargin       = 4;
constexpr std::size_t kMaxQueued    = 8;

// 一次打开请求：主线程写入，工作线程按 FIFO 逐条取出。
struct Request {
    HWND          owner{};
    RECT          box{};
    std::string   text;
    std::size_t   maxBytes{};
    std::uint64_t session{};
    int           field{};
    int           fontPx{};
    int           textInset{};
};

// 跨线程共享：只在锁内读写。
std::mutex                   g_mutex;
std::deque<Request>          g_pending;
std::deque<NativeTextResult> g_results;
NativeTextPreview            g_preview;
bool                         g_previewValid{};
RECT                         g_position{};
RECT                         g_appliedPosition{};
bool                         g_hasPosition{};
bool                         g_positionPosted{};
bool                         g_closePosted{};
std::atomic<bool>            g_active{false};
std::atomic<HWND>            g_hostAtomic{nullptr};
std::atomic<HWND>            g_popupAtomic{nullptr};
std::atomic<bool>            g_ready{false};
HANDLE                       g_startedEvent{};
std::thread                  g_worker;

// 仅工作线程访问。
struct Active {
    HWND          owner{};
    std::uint64_t session{};
    int           field{};
    std::string   originalText;
    std::size_t   maxBytes{};
    bool          open{};
};
Active  g_current;
HWND    g_popup{};
HWND    g_edit{};
WNDPROC g_editProc{};
HFONT   g_font{};
HBRUSH  g_bgBrush{};
HBRUSH  g_borderBrush{};
bool    g_closing{};
int     g_fontPx{};
int     g_textInset{};

void pushPreview(std::string text, int caret, int anchor);
void pushPreviewWithSelection(std::string text);
bool    g_focused{};

// 模组内取自身 DLL 句柄；standalone host 里退回主模块。
HMODULE hostModule() {
    HMODULE module = GetModuleHandleW(L"ServerListEnhancer.dll");
    return module ? module : GetModuleHandleW(nullptr);
}

std::string truncateUtf8(std::string const& text, std::size_t maxBytes) {
    if (maxBytes == 0 || text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut);
}

void pushPreview(std::string text, int caret, int anchor) {
    if (!g_current.open) return;
    std::lock_guard lock(g_mutex);
    g_preview      = NativeTextPreview{g_current.session, g_current.field, std::move(text), caret, anchor};
    g_previewValid = true;
}

std::wstring readEditWide() {
    if (!g_edit || !IsWindow(g_edit)) return {};
    int const length = GetWindowTextLengthW(g_edit);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
    int const    copied = GetWindowTextW(g_edit, wide.data(), length + 1);
    wide.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    return wide;
}

// UTF-16 偏移 -> UTF-8 字节偏移；落在代理对中间时退回到该码点起点。
int utf8OffsetForUtf16(std::wstring const& wide, int utf16Offset) {
    int bytes = 0;
    std::size_t const limit = std::min(wide.size(), static_cast<std::size_t>(std::max(0, utf16Offset)));
    for (std::size_t i = 0; i < limit;) {
        unsigned const code = static_cast<unsigned>(wide[i]);
        if (code >= 0xD800 && code <= 0xDBFF && i + 1 < wide.size()
            && static_cast<unsigned>(wide[i + 1]) >= 0xDC00
            && static_cast<unsigned>(wide[i + 1]) <= 0xDFFF) {
            if (i + 2 > limit) break;
            bytes += 4;
            i += 2;
        } else {
            bytes += code < 0x80 ? 1 : code < 0x800 ? 2 : 3;
            ++i;
        }
    }
    return bytes;
}

void pushPreviewWithSelection(std::string text) {
    int caret  = static_cast<int>(text.size());
    int anchor = caret;
    if (g_edit && IsWindow(g_edit)) {
        DWORD selStart{};
        DWORD selEnd{};
        SendMessageW(g_edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart), reinterpret_cast<LPARAM>(&selEnd));
        std::wstring const wide = readEditWide();
        anchor                  = utf8OffsetForUtf16(wide, static_cast<int>(selStart));
        caret                   = utf8OffsetForUtf16(wide, static_cast<int>(selEnd));
    }
    pushPreview(std::move(text), caret, anchor);
}

std::string readEditText() {
    if (!g_edit || !IsWindow(g_edit)) return {};
    int const length = GetWindowTextLengthW(g_edit);
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
    int const    copied = GetWindowTextW(g_edit, wide.data(), length + 1);
    wide.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0);
    if (wide.empty()) return {};
    int const bytes = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr
    );
    if (bytes <= 0) return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), bytes, nullptr, nullptr
    );
    return out;
}

bool isComposing() {
    HWND const target = g_edit && IsWindow(g_edit) ? g_edit : g_popup;
    if (!target) return false;
    HIMC const imc = ImmGetContext(target);
    if (!imc) return false;
    LONG const length = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(target, imc);
    return length > 0;
}

void refreshActive() {
    std::lock_guard lock(g_mutex);
    if (!g_current.open && g_pending.empty()) g_active.store(false, std::memory_order_release);
}

void destroyWindow() {
    if (g_closing) return;
    g_closing = true;

    HWND const edit  = g_edit;
    HWND const popup = g_popup;
    g_edit  = nullptr;
    g_popup = nullptr;
    g_popupAtomic.store(nullptr, std::memory_order_release);

    if (edit && IsWindow(edit) && g_editProc) {
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_editProc));
    }
    g_editProc = nullptr;

    if (popup && IsWindow(popup)) DestroyWindow(popup);
    if (g_font) {
        DeleteObject(g_font);
        g_font = nullptr;
    }
    g_current = Active{};
    g_focused   = false;
    g_closing = false;
}

void queueResult(NativeTextAction action) {
    NativeTextResult result;
    result.session = g_current.session;
    result.field   = g_current.field;
    result.action  = action;
    result.text =
        truncateUtf8(action == NativeTextAction::Cancel ? g_current.originalText : readEditText(), g_current.maxBytes);
    std::lock_guard lock(g_mutex);
    if (g_results.size() >= kMaxQueued) g_results.pop_front();
    g_results.push_back(std::move(result));
}

// 结束当前编辑：结果按 g_current 身份入队，与排队中的下一个请求无关。
void finish(NativeTextAction action) {
    if (!g_current.open || g_closing) return;
    queueResult(action);
    destroyWindow();
    refreshActive();
}

// 程序侧关闭：不产生结果。
void discardCurrent() {
    if (!g_current.open) return;
    destroyWindow();
    refreshActive();
}

void handleFocusLost() {
    if (g_closing || !g_current.open) return;
    if (isComposing()) return; // 组字中不打断，避免丢拼音
    finish(NativeTextAction::Blur);
}

void layoutEdit(int width, int height) {
    if (!g_edit) return;
    int const inset      = std::max(0, g_textInset);
    int const editHeight = std::min(std::max(12, height - kBorder * 2), std::max(12, g_fontPx + 6));
    int const editTop    = std::max(kBorder, (height - editHeight) / 2);
    SetWindowPos(
        g_edit,
        nullptr,
        inset,
        editTop,
        std::max(10, width - inset * 2),
        editHeight,
        SWP_NOZORDER | SWP_NOACTIVATE
    );
}

void applyPosition(RECT const& box) {
    if (!g_popup || !IsWindow(g_popup)) return;
    int const width  = box.right - box.left;
    int const height = box.bottom - box.top;
    if (width < 10 || height < 10) return;
    SetWindowPos(g_popup, HWND_TOPMOST, box.left, box.top, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    layoutEdit(width, height);
}

LRESULT CALLBACK editProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        if (wParam >= VK_F1 && wParam <= VK_F24) {
            if (g_current.owner) PostMessageW(g_current.owner, msg, wParam, lParam);
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (isComposing()) return CallWindowProcW(g_editProc, hwnd, msg, wParam, lParam);
            finish(NativeTextAction::Commit);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (isComposing()) return CallWindowProcW(g_editProc, hwnd, msg, wParam, lParam);
            finish(NativeTextAction::Cancel);
            return 0;
        }
        if (wParam == VK_TAB) {
            finish(NativeTextAction::Tab);
            return 0;
        }
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            pushPreviewWithSelection(readEditText());
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME || wParam == VK_END
            || wParam == VK_UP || wParam == VK_DOWN) {
            LRESULT const handled =
                g_editProc ? CallWindowProcW(g_editProc, hwnd, msg, wParam, lParam) : 0;
            pushPreviewWithSelection(readEditText());
            return handled;
        }
    } else if (msg == WM_SETFOCUS) {
        g_focused = true;
    } else if (msg == WM_IME_COMPOSITION) {
        if (lParam & GCS_COMPSTR) {
            std::string combined = readEditText();
            HIMC const  imc      = ImmGetContext(hwnd);
            if (imc) {
                LONG const bytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
                if (bytes > 0) {
                    std::wstring comp(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(imc, GCS_COMPSTR, comp.data(), bytes);
                    int const needed = WideCharToMultiByte(
                        CP_UTF8, 0, comp.c_str(), static_cast<int>(comp.size()), nullptr, 0, nullptr, nullptr
                    );
                    if (needed > 0) {
                        std::string utf8(static_cast<std::size_t>(needed), '\0');
                        WideCharToMultiByte(
                            CP_UTF8, 0, comp.c_str(), static_cast<int>(comp.size()), utf8.data(), needed, nullptr, nullptr
                        );
                        combined += utf8;
                    }
                }
                ImmReleaseContext(hwnd, imc);
            }
            int const previewEnd = static_cast<int>(combined.size());
            pushPreview(std::move(combined), previewEnd, previewEnd);
        }
    } else if (msg == WM_KILLFOCUS) {
        HWND const next = reinterpret_cast<HWND>(wParam);
        if (g_focused && next != g_popup && next != g_edit) handleFocusLost();
    }
    return g_editProc ? CallWindowProcW(g_editProc, hwnd, msg, wParam, lParam)
                      : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC const hdc   = reinterpret_cast<HDC>(wParam);
        HGDIOBJ   brush = g_bgBrush ? reinterpret_cast<HGDIOBJ>(g_bgBrush) : GetStockObject(BLACK_BRUSH);
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(22, 22, 22));
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC const   hdc = BeginPaint(hwnd, &paint);
        RECT        client{};
        GetClientRect(hwnd, &client);
        if (g_bgBrush) FillRect(hdc, &client, g_bgBrush);
        if (g_borderBrush) {
            RECT top{client.left, client.top, client.right, client.top + kBorder};
            RECT bottom{client.left, client.bottom - kBorder, client.right, client.bottom};
            RECT left{client.left, client.top, client.left + kBorder, client.bottom};
            RECT right{client.right - kBorder, client.top, client.right, client.bottom};
            FillRect(hdc, &top, g_borderBrush);
            FillRect(hdc, &bottom, g_borderBrush);
            FillRect(hdc, &left, g_borderBrush);
            FillRect(hdc, &right, g_borderBrush);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case kMsgActivate:
        if (g_edit) SetFocus(g_edit);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE) pushPreviewWithSelection(readEditText());
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_IBEAM));
        return TRUE;
    case WM_SETFOCUS:
        g_focused = true;
        break;
    case WM_KILLFOCUS: {
        HWND const next = reinterpret_cast<HWND>(wParam);
        if (g_focused && next != g_popup && next != g_edit) handleFocusLost();
        break;
    }
    case WM_CLOSE:
        finish(NativeTextAction::Cancel);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void createWindow(Request const& request) {
    int const width  = request.box.right - request.box.left;
    int const height = request.box.bottom - request.box.top;
    if (width < 10 || height < 10) {
        refreshActive();
        return;
    }

    if (!g_bgBrush) g_bgBrush = CreateSolidBrush(RGB(22, 22, 22));
    if (!g_borderBrush) g_borderBrush = CreateSolidBrush(RGB(169, 218, 142));

    HINSTANCE const instance = hostModule();
    HWND const      popup    = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kPopupClass,
        L"",
        WS_POPUP | WS_CLIPCHILDREN,
        request.box.left,
        request.box.top,
        width,
        height,
        request.owner,
        nullptr,
        instance,
        nullptr
    );
    if (!popup) {
        refreshActive();
        return;
    }
    g_popup = popup;
    // 全透明：文字与光标一律由调用方用游戏字体绘制，原生控件只负责 IME。
    SetLayeredWindowAttributes(popup, 0, 0, LWA_ALPHA);

    g_fontPx    = request.fontPx > 0
                             ? request.fontPx
                             : std::max(9, (height - kBorder * 2) * 3 / 4);
    g_textInset = std::max(0, request.textInset);
    g_font = CreateFontW(
        -g_fontPx,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei"
    );

    HWND const edit = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
        std::max(0, g_textInset),
        kBorder,
        std::max(10, width - std::max(0, g_textInset) * 2),
        std::max(12, g_fontPx + 6),
        popup,
        reinterpret_cast<HMENU>(1),
        instance,
        nullptr
    );
    if (!edit) {
        destroyWindow();
        refreshActive();
        return;
    }
    g_edit = edit;

    if (request.maxBytes > 0) SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(request.maxBytes), 0);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    if (g_font) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), MAKELPARAM(TRUE, 0));
    layoutEdit(width, height);

    if (!request.text.empty()) {
        int const wideLength =
            MultiByteToWideChar(CP_UTF8, 0, request.text.c_str(), static_cast<int>(request.text.size()), nullptr, 0);
        if (wideLength > 0) {
            std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
            MultiByteToWideChar(
                CP_UTF8, 0, request.text.c_str(), static_cast<int>(request.text.size()), wide.data(), wideLength
            );
            SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        }
    }
    int const textLength = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(textLength), static_cast<LPARAM>(textLength));

    g_editProc =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&editProc)));

    // 身份在窗口创建成功后一次写入；本段编辑的所有结果都用它。
    g_current.owner        = request.owner;
    g_current.session      = request.session;
    g_current.field        = request.field;
    g_current.originalText = request.text;
    g_current.maxBytes     = request.maxBytes;
    g_current.open         = true;
    pushPreviewWithSelection(request.text);
    g_active.store(true, std::memory_order_release);

    g_popupAtomic.store(popup, std::memory_order_release);
    ShowWindow(popup, SW_SHOWNOACTIVATE);
    UpdateWindow(popup);
}

void handleOpen() {
    Request request;
    {
        std::lock_guard lock(g_mutex);
        if (g_pending.empty()) {
            if (!g_current.open) g_active.store(false, std::memory_order_release);
            return;
        }
        request = std::move(g_pending.front());
        g_pending.pop_front();
    }
    // 先按旧身份结束上一段编辑，再创建新窗口。
    if (g_current.open) finish(NativeTextAction::Blur);
    createWindow(request);
}

void handlePosition() {
    RECT box{};
    bool has{};
    {
        std::lock_guard lock(g_mutex);
        box = g_position;
        has = g_hasPosition;
        g_hasPosition = false;
        g_positionPosted = false;
    }
    if (!has) return;
    applyPosition(box);
    std::lock_guard lock(g_mutex);
    g_appliedPosition = box;
}

// 隐藏宿主窗口：所有跨线程投递都落到它上面，由工作线程的消息泵处理。
LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case kMsgOpen:
        {
            std::lock_guard lock(g_mutex);
            g_closePosted = false;
        }
        handleOpen();
        return 0;
    case kMsgClose:
        {
            std::lock_guard lock(g_mutex);
            g_closePosted = false;
        }
        discardCurrent();
        return 0;
    case kMsgPosition:
        handlePosition();
        return 0;
    case kMsgCommit:
        finish(NativeTextAction::Blur);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool registerClasses() {
    HINSTANCE const instance = hostModule();

    WNDCLASSEXW popup{};
    popup.cbSize        = sizeof(popup);
    popup.style         = CS_HREDRAW | CS_VREDRAW;
    popup.lpfnWndProc   = &popupProc;
    popup.hInstance     = instance;
    popup.hCursor       = LoadCursor(nullptr, IDC_IBEAM);
    popup.lpszClassName = kPopupClass;
    if (!RegisterClassExW(&popup) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW host{};
    host.cbSize        = sizeof(host);
    host.lpfnWndProc   = &hostProc;
    host.hInstance     = instance;
    host.lpszClassName = kHostClass;
    if (!RegisterClassExW(&host) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    return true;
}

void workerMain() {
    MSG probe{};
    PeekMessageW(&probe, nullptr, 0, 0, PM_NOREMOVE);

    // TSF/输入法上下文要求 STA 线程；失败也不影响纯 IMM 路径。
    HRESULT const com     = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool const    classes = registerClasses();

    HWND host{};
    if (classes) {
        host = CreateWindowExW(
            0, kHostClass, L"", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hostModule(), nullptr
        );
    }
    g_hostAtomic.store(host, std::memory_order_release);
    g_ready.store(host != nullptr, std::memory_order_release);
    if (g_startedEvent) SetEvent(g_startedEvent);
    if (!host) {
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    discardCurrent();
    if (g_bgBrush) {
        DeleteObject(g_bgBrush);
        g_bgBrush = nullptr;
    }
    if (g_borderBrush) {
        DeleteObject(g_borderBrush);
        g_borderBrush = nullptr;
    }
    UnregisterClassW(kPopupClass, hostModule());
    UnregisterClassW(kHostClass, hostModule());
    {
        std::lock_guard lock(g_mutex);
        g_pending.clear();
        g_active.store(false, std::memory_order_release);
    }
    g_hostAtomic.store(nullptr, std::memory_order_release);
    if (SUCCEEDED(com)) CoUninitialize();
}

bool ensureWorker() {
    if (g_worker.joinable()) return true;
    if (!g_startedEvent) {
        g_startedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_startedEvent) return false;
    }
    ResetEvent(g_startedEvent);
    g_worker = std::thread(&workerMain);
    if (!g_worker.joinable()) return false;
    WaitForSingleObject(g_startedEvent, 2000);
    if (!g_ready.load(std::memory_order_acquire)) {
        if (g_worker.joinable()) g_worker.join();
        return false;
    }
    return true;
}

} // namespace

bool openNativeTextInput(
    HWND               owner,
    RECT               screenBox,
    std::string const& text,
    std::size_t        maxBytes,
    std::uint64_t      session,
    int                field,
    int                fontPixelHeight,
    int                textInsetPixels
) {
    if (!ensureWorker()) return false;
    HWND const host = g_hostAtomic.load(std::memory_order_acquire);
    if (!host) return false;

    {
        std::lock_guard lock(g_mutex);
        if (g_pending.size() >= kMaxQueued) g_pending.pop_front();
        g_pending.push_back(Request{owner, screenBox, text, maxBytes, session, field, fontPixelHeight, textInsetPixels});
        g_position    = screenBox;
        g_hasPosition = false;
        // 从入队起即算活跃：窗口创建前 UI 也不能回落到轮询输入，否则会吞拼音。
        g_active.store(true, std::memory_order_release);
    }
    DWORD_PTR ignored{};
    auto const posted = SendMessageTimeoutW(host, kMsgOpen, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &ignored);
    if (!posted) return false;
    // 前台激活必须在拥有当前前台窗口的调用线程做，工作线程里 SetForegroundWindow 会长时间阻塞。
    HWND const popup = g_popupAtomic.load(std::memory_order_acquire);
    if (popup) {
        SetForegroundWindow(popup);
        PostMessageW(popup, kMsgActivate, 0, 0);
    }
    return true;
}

void positionNativeTextInput(RECT screenBox) {
    HWND const host = g_hostAtomic.load(std::memory_order_acquire);
    if (!host) return;
    bool needPost = false;
    {
        std::lock_guard lock(g_mutex);
        // 位置是幂等的：同一矩形不重复投递；有未处理请求时只更新最新值。
        if (g_hasPosition && EqualRect(&g_position, &screenBox)) return;
        if (!g_hasPosition && !g_positionPosted && EqualRect(&g_appliedPosition, &screenBox)) return;
        g_position    = screenBox;
        g_hasPosition = true;
        if (!g_positionPosted) {
            g_positionPosted = true;
            needPost         = true;
        }
    }
    // 纯异步：渲染线程绝不能等 work 线程（后台省电模式下会被降频放大成掉帧）。
    if (needPost) PostMessageW(host, kMsgPosition, 0, 0);
}

bool nativeTextInputActive() { return g_active.load(std::memory_order_acquire); }

std::optional<NativeTextPreview> takeNativeTextPreview() {
    std::lock_guard lock(g_mutex);
    if (!g_previewValid) return std::nullopt;
    g_previewValid = false;
    return g_preview;
}

std::optional<NativeTextResult> takeNativeTextInput() {
    std::lock_guard lock(g_mutex);
    if (g_results.empty()) return std::nullopt;
    NativeTextResult out = std::move(g_results.front());
    g_results.pop_front();
    return out;
}

void commitNativeTextInput() {
    HWND const host = g_hostAtomic.load(std::memory_order_acquire);
    if (!host) return;
    // 不持 g_mutex：目标线程的 finish() 会自己加锁。
    DWORD_PTR ignored{};
    SendMessageTimeoutW(host, kMsgCommit, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
}

void closeNativeTextInput() {
    HWND const host = g_hostAtomic.load(std::memory_order_acquire);
    if (!host) return;
    {
        std::lock_guard lock(g_mutex);
        if (g_closePosted) return;
        g_closePosted = true;
    }
    PostMessageW(host, kMsgClose, 0, 0);
}

void shutdownNativeTextInput() {
    HWND const host = g_hostAtomic.load(std::memory_order_acquire);
    if (host) PostMessageW(host, WM_CLOSE, 0, 0);
    if (g_worker.joinable()) g_worker.join();
    g_hostAtomic.store(nullptr, std::memory_order_release);
    if (g_startedEvent) {
        CloseHandle(g_startedEvent);
        g_startedEvent = nullptr;
    }
    std::lock_guard lock(g_mutex);
    g_pending.clear();
    g_results.clear();
    g_previewValid = false;
    g_active.store(false, std::memory_order_release);
}

} // namespace sle::client
