#include "OreUIBridge.h"

#include <windows.h>

#include "bridge/DearOreUIBridge.h" // 仅取类型定义；函数符号运行时解析

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"

namespace sle::client {

namespace {

using QueryApiFn    = DearOreUIBridgeResult (*)(unsigned int);
using RemountAllFn  = int (*)();
using ExecuteScriptFn = int (*)(char const*);

constexpr wchar_t kModuleName[] = L"DearOreUI.dll";
constexpr char    kExportName[] = "DearOreUI_QueryApi";

} // namespace

OreUIBridge& OreUIBridge::getInstance() {
    static OreUIBridge instance;
    return instance;
}

bool OreUIBridge::attach() {
    if (mApi != nullptr) {
        return true;
    }
    auto& logger = ll::mod::NativeMod::current()->getLogger();

    HMODULE module = ::GetModuleHandleW(kModuleName);
    if (module == nullptr) {
        module = ::LoadLibraryW(kModuleName);
    }
    if (module == nullptr) {
        mLastError = "DearOreUI.dll 未加载";
        logger.warn("未检测到 DearOreUI 前置（{}），跳过 OreUI 桥接", mLastError);
        return false;
    }
    mModule = module;

    auto query = reinterpret_cast<QueryApiFn>(::GetProcAddress(module, kExportName));
    if (query == nullptr) {
        mLastError = "DearOreUI.dll 缺少导出 DearOreUI_QueryApi";
        logger.warn("{}", mLastError);
        return false;
    }

    DearOreUIBridgeResult result = query(0);
    if (result.status != DearOreUIBridge_Ok || result.api == nullptr) {
        mLastError = "DearOreUI 未就绪, status=" + std::to_string(result.status)
                   + ", protocol=" + std::to_string(result.protocolVersion);
        logger.warn("{}", mLastError);
        return false;
    }

    mApi = result.api;
    logger.info("已连接 DearOreUI API (protocol={})", result.protocolVersion);
    return true;
}

int OreUIBridge::executeScript(std::string const& js) {
    if (mModule == nullptr) {
        return -4;
    }
    auto exec = reinterpret_cast<ExecuteScriptFn>(::GetProcAddress(static_cast<HMODULE>(mModule), "DearOreUI_ExecuteScript"));
    if (exec == nullptr) {
        return -4;
    }
    return exec(js.c_str());
}

int OreUIBridge::remountAll() {
    if (mModule == nullptr) {
        return 0;
    }
    auto remount = reinterpret_cast<RemountAllFn>(::GetProcAddress(static_cast<HMODULE>(mModule), "DearOreUI_RemountAll"));
    if (remount == nullptr) {
        return -1; // 前置未包含补挂导出
    }
    return remount();
}

} // namespace sle::client
