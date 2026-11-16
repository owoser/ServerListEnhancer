# ServerListEnhancer（服务器列表增强）

基岩版 1.26.60 + LeviLamina 客户端模组：在**主菜单**添加一个独立的服务器列表入口，自绘 UI 显示服务器 Logo、名称、简介与在线状态，不依赖原版服务器列表。

> 当前状态：UI 骨架与视觉档案链路已可用；「点击进服」阻塞在 MCAPI（见 [docs/mcapi-request.md](docs/mcapi-request.md)），其余功能按计划推进中。

## 功能

- 主菜单菜单列顶部的「服务器列表」按钮（原版风格：144×26 UI、配色与字号对齐原版按钮）。
- 自绘列表页：条目标题、简介、地址、在线人数、Logo。
- 服务端视觉档案：装了本模组服务端插件时，服务端通过自定义包下发 `serverId`、`logoUrl`/`logoId`、`introText`，客户端缓存并覆盖展示。
- 没装服务端插件时回落到服务器状态查询（NetherNet `/v1/join`）的 `name` 与内置图片。
- 只在主菜单绘制：任何弹窗/加载/游戏内屏幕都不绘制（按场景栈全栈判定）。

## 目录结构

```
xmake.lua                    构建脚本（--target_type=server|client）
manifest.json                模组清单
src/common/                  两端共用：VisualSpec / Validation / ServerVisualPacket
src-server/                  服务端：配置、/sle 命令、PlayerJoinEvent 下发
src-client/                  客户端：ClientEntry / ProbeOverlay(UI) / VisualCache
                             VisualPacketHandler / SessionTracker
docs/mcapi-request.md        MCAPI 符号申请草稿（joinExternalServer 等）
```

## 构建

需要 xmake、clang-cl（LLVM）与 LeviLamina 的 xmake-repo。

```powershell
# 客户端模组
xmake f --target_type=client -y
xmake -y

# 服务端插件
xmake f --target_type=server -y
xmake -y
```

产物：`bin\ServerListEnhancer\ServerListEnhancer.dll`。服务端与客户端的 configure 不能并发，切换端后需重新 configure。

## 安装

把 `bin\ServerListEnhancer\` 目录整体复制到客户端的 `mods\ServerListEnhancer\`（或服务端 `mods\`）。
客户端 UI 依赖 LeviLamina 客户端版；部署前必须完全退出游戏，否则 dll 被占用无法覆盖。

## 已知限制

- 进服接口 `World::ServerWorldJoiner::joinExternalServer` 等在当前 LeviLamina 版本为 MCNAPI（不可链接），已整理申请草稿；在批准前列表点击仅提示。
- overlay 依赖自绘，无法与 HBUI 原生控件合并，因此按钮与面板是独立绘制层。
- 中文输入法暂不支持（自绘输入框仅处理可打印 ASCII）。

## 许可

MIT
