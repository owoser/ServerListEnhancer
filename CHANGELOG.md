# Changelog

## [26.60.8002] - 2026-11-19

撤回并重新发布 26.60.8001：该版本的标签与发布包重建，发行序号按规则递增到 2（版本号 26.60.8002）。构建仍基于 LeviLamina 26.60.8。

### Changed

- 服务端 lip 依赖从精确 `26.60.8` 放宽为 `>=26.60.2`。服务端没有运行期版本门禁，同一份源码在 26.60.2 SDK 下也能编译并链接（prelink 消费 `levilamina\26.60.2\...\LeviLamina.lib`），因此在 LL 26.60.2 的服务器工作区可以直接 `lip install`。Windows 客户端仍精确要求 `26.60.8`：加入游戏用到的 `World::ServerWorldJoiner::joinExternalServer` 与运行门禁都在该版本。

### Fixed

- 修复在 LeviLamina 26.60.2 的服务器工作区安装时被 lip 依赖求解拒绝的问题（`Cannot find versions of the packages that work together`）。

## [26.60.8001] - 2026-11-19

构建基于 LeviLamina 26.60.8。全平台统一使用三段版本号 26.60.8001（LL 补丁 8 × 1000 + 发行序号 1），Git 标签为 v26.60.8001；此前的四段显示号 26.60.8.1 已废弃，因为 lip 的 SemVer 解析器拒绝四段版本。

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
