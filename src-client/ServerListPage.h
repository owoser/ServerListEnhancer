#pragma once

namespace sle::client {

// 通过 DearOreUI 把「服务器列表」面板渲染成 OreUI 页面的 DOM（HTML/CSS）。
bool installServerListPage();
void uninstallServerListPage();

// 重新生成面板 HTML（数据变化时调用）。
void refreshServerListPage();

// 当前视口从第几条开始显示（翻页时调用）。
void setServerListScroll(int firstIndex);

// 面板是否已挂载到当前页面（用于让 C++ 只画交互层）。
bool htmlListActive();

// 每帧调用（渲染期）：在 DearOreUI 就绪前重试注册。
void pumpServerListPage();

// 直接把当前列表推到 OreUI 页面（直接 ExecuteScript，不依赖页面挂载时序）。
void pushListToPage();

} // namespace sle::client
