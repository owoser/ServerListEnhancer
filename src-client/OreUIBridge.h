#pragma once

#include <string>

namespace sle::client {

// 通过 DearOreUI.dll 导出的纯 C ABI（DearOreUI_QueryApi）取得 API 实例。
// DearOreUI 是可选前置：没有它时本类只报告未就绪，不影响模组其它功能。
class OreUIBridge {
public:
    static OreUIBridge& getInstance();

    // 每次调用都会尝试一次（GetModuleHandleW + GetProcAddress 很便宜），
    // 成功后缓存 API 指针；由调用方控制重试节奏。
    bool attach();

    // 让 DearOreUI 为「已存在的页面上下文」补做一次挂载（注册晚于页面创建时用）。
    int remountAll();

    // 直接对当前 cohtml::View 执行一段 JS。返回 0 成功，-1 无 view，
    // -2 脚本上下文未就绪，-3 槽位不可用，-4 前置缺少导出。
    int executeScript(std::string const& js);

    [[nodiscard]] void* api() const { return mApi; }
    [[nodiscard]] bool  ready() const { return mApi != nullptr; }
    [[nodiscard]] std::string const& lastError() const { return mLastError; }

private:
    OreUIBridge() = default;

    void*       mApi{};
    void*       mModule{};
    std::string mLastError;
};

} // namespace sle::client
