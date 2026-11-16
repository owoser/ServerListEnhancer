#pragma once

namespace sle::client {

// P1 探针：在首页绘制一个红色按钮，用来验证
// 1) AfterUIRenderEvent 的绘制是否覆盖在 HBUI(WebView) 之上
// 2) 鼠标事件的坐标系是否与绘制坐标系一致（点击是否命中）
void registerProbeOverlay();
void unregisterProbeOverlay();

} // namespace sle::client
