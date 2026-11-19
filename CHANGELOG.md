# Changelog

## [26.60.8.1] - 2026-11-19

构建基于 LeviLamina 26.60.8。展示版本为 26.60.8.1；lip 和加载器包版本为 26.60.8001，Git 标签为 v26.60.8001。

### Added

- 发布 Windows x64 客户端、配套 Windows x64 服务端和 Android ARM64 安装包。
- 通过 lip 0.41 的 format 4 配置安装 Windows 双端包，并保留已有服务器列表、配置和缓存。
- Windows 与 Android 的添加/编辑服务器表单支持中文输入法组合输入、候选词与 UTF-8 安全编辑。
- 服务器列表支持图文简介、GIF 动画、真实 UDP 延迟，以及加入游戏。

### Fixed

- Windows 编辑中与结束编辑后使用同一游戏字库，修复字体与字号跳变。
- Windows 输入法窗口定位和关闭异步处理，修复选中输入框后卡顿。
- Android 文字和光标统一字宽与字体基线，修复中文光标偏远和偏上；地址字段位于名称字段上方。
- 修复进服退出后重开列表的 GIF 动画，以及 Android GUI 缩放后的触摸命中与绿色按压反馈。
- 默认关闭鼠标、命中与 GIF 调试日志；图文统一从 /v1/join 发现。

### Notes

- Windows 验证游戏版本为 1.26.60.05，要求 LeviLamina 26.60.8；Android 使用匹配 LeviLauncher 1.6.0 的 Preloader SDK 与 NDK 28.2。
- 客户端和服务端 DLL 不可混用；Android 应导入 .levipack。
- Windows 支持方向键、Shift 选区与 Ctrl+A；鼠标拖选尚未实现。
