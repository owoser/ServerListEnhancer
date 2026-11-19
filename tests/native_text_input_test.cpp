#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../src-client/NativeTextInput.h"
#include <cassert>
#include <chrono>
#include <functional>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace sle::client;
namespace {
void pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
bool until(std::function<bool()> predicate) {
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        pump();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
HWND editWindow() {
    HWND const popup = FindWindowW(L"ServerListEnhancer.NativeTextInput", nullptr);
    return popup ? GetDlgItem(popup, 1) : nullptr;
}
NativeTextResult finish(HWND edit, UINT key) {
    PostMessageW(edit, WM_KEYDOWN, key, 0);
    std::optional<NativeTextResult> result;
    assert(until([&] { result = takeNativeTextInput(); return result.has_value(); }));
    return std::move(*result);
}

// 性能回归用：阻塞 worker 线程的窗口过程。
constexpr UINT kBlockWorker = WM_APP + 77;
WNDPROC       g_baseEditProc{};
LRESULT CALLBACK blockingEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == kBlockWorker) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        return 0;
    }
    return CallWindowProcW(g_baseEditProc, hwnd, msg, wParam, lParam);
}
HWND open(HWND owner, std::string const& text, std::size_t limit, int field, std::uint64_t session=73) {
    assert(openNativeTextInput(owner, {100,100,600,140}, text, limit, session, field));
    assert(until([] { return editWindow() && IsWindowVisible(editWindow()); }));
    return editWindow();
}
}
int main() {
    // Third-party IMEs may persist files relative to the test process CWD.
    // Keep all of that runtime state beside the build output, never in the repo root.
    wchar_t executable[32768]{};
    assert(GetModuleFileNameW(nullptr, executable, 32768));
    auto const runtime = std::filesystem::path(executable).parent_path() / "native-ime-runtime";
    std::error_code error;
    std::filesystem::create_directories(runtime, error);
    assert(!error);
    std::filesystem::current_path(runtime, error);
    assert(!error);
    auto const english = LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE | KLF_SETFORPROCESS);
    (void)english; // Unicode/bridge tests do not need an external IME. Real composition is tested in-game.
    HWND const owner = CreateWindowExW(0, L"STATIC", L"SLE native input test", WS_OVERLAPPEDWINDOW,
                                       50,50,800,400, nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    assert(owner);
    ShowWindow(owner, SW_SHOWNORMAL);
    auto edit = open(owner, "", 128, 1);
    // Send UTF-16 through the same native EDIT used by the system's IME.
    // This verifies the bridge; real composition/candidate UI is a separate live check.
    SendMessageW(edit, WM_CHAR, L'中', 0);
    SendMessageW(edit, WM_CHAR, L'文', 0);
    auto result = finish(edit, VK_RETURN);
    assert(result.session==73 && result.field==1 && result.text=="中文");
    assert(result.action==NativeTextAction::Commit);

    edit = open(owner, "旧名称", 128, 1);
    SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"新名称"));
    result = finish(edit, VK_ESCAPE);
    assert(result.text=="旧名称" && result.action==NativeTextAction::Cancel);

    edit = open(owner, std::string(126,'x'), 128, 1);
    SendMessageW(edit, WM_CHAR, L'中', 0);
    result = finish(edit, VK_TAB);
    assert(result.text==std::string(126,'x') && result.action==NativeTextAction::Tab);

    edit = open(owner, "旧地址", 255, 0, 81);
    SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"127.0.0.1:19132"));
    assert(openNativeTextInput(owner, {100,180,600,220}, "新名称", 128, 82, 1));
    std::optional<NativeTextResult> old;
    assert(until([&] { old=takeNativeTextInput(); return old.has_value(); }));
    assert(old->session==81 && old->field==0 && old->text=="127.0.0.1:19132");
    assert(until([] { return editWindow() && IsWindowVisible(editWindow()); }));
    commitNativeTextInput();
    std::optional<NativeTextResult> current;
    assert(until([&] { current=takeNativeTextInput(); return current.has_value(); }));
    assert(current->session==82 && current->field==1 && current->text=="新名称");

    edit = open(owner, "保留", 128, 1);
    positionNativeTextInput({170,210,700,260});
    assert(until([] {
        HWND const popup=FindWindowW(L"ServerListEnhancer.NativeTextInput",nullptr);
        RECT box{};
        return popup && GetWindowRect(popup,&box) && box.left==170 && box.top==210
               && box.right==700 && box.bottom==260;
    }));

    // 预览快照的 caret/anchor 必须是 UTF-8 字节偏移（"中文" = 6 字节）。
    edit = open(owner, "中文", 128, 1);
    {
        std::optional<NativeTextPreview> preview;
        assert(until([&] { preview = takeNativeTextPreview(); return preview.has_value(); }));
        assert(preview->caret == 6 && preview->anchor == 6);
        PostMessageW(edit, WM_KEYDOWN, VK_HOME, 0);
        assert(until([&] {
            preview = takeNativeTextPreview();
            return preview && preview->caret == 0 && preview->anchor == 0;
        }));
        PostMessageW(edit, WM_KEYDOWN, VK_END, 0);
        assert(until([&] {
            preview = takeNativeTextPreview();
            return preview && preview->caret == 6 && preview->anchor == 6;
        }));
        while (takeNativeTextInput()) {
        } // 丢弃前面用例可能残留的 Blur
        auto const previewResult = finish(edit, VK_ESCAPE);
        assert(previewResult.text == "中文");
    }

    // A surrogate pair consumes two UTF-16 units but one UTF-8 code point.
    edit = open(owner, "\xf0\x9f\x99\x82中文A", 128, 1);
    {
        SendMessageW(edit, EM_SETSEL, 2, 2); // after the emoji
        PostMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
        std::optional<NativeTextPreview> preview;
        assert(until([&] {
            preview = takeNativeTextPreview();
            return preview && preview->caret == 7 && preview->anchor == 7;
        }));
        PostMessageW(edit, WM_KEYDOWN, VK_RIGHT, 0);
        assert(until([&] {
            preview = takeNativeTextPreview();
            return preview && preview->caret == 10 && preview->anchor == 10;
        }));
        while (takeNativeTextInput()) {}
        auto const unicodeResult = finish(edit, VK_ESCAPE);
        assert(unicodeResult.text == "\xf0\x9f\x99\x82中文A");
    }

    // 性能回归：worker 被阻塞 400ms 时，position/close 必须立即返回。
    // 旧的同步实现（position=SendMessageTimeout 100ms、close=500ms）会在此失败。
    edit = open(owner, "perf", 128, 1);
    g_baseEditProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&blockingEditProc)));
    assert(g_baseEditProc);
    PostMessageW(edit, kBlockWorker, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto const start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) positionNativeTextInput({100 + i, 100 + i, 600 + i, 140 + i});
    closeNativeTextInput();
    auto const elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    assert(elapsed < 100);
    SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_baseEditProc));

    assert(until([] { return !nativeTextInputActive(); }));
    assert(!takeNativeTextInput());
    shutdownNativeTextInput();
    assert(!FindWindowW(L"ServerListEnhancer.NativeTextInput",nullptr));
    edit = open(owner, "重启", 128, 1);
    result = finish(edit, VK_RETURN);
    assert(result.text=="重启");
    shutdownNativeTextInput();
    DestroyWindow(owner);
    std::cout << "Native EDIT Unicode, cancel, byte limits, session isolation, geometry, shutdown/restart: PASS\n";
}
