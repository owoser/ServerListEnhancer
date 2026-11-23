# ServerListEnhancer（服务器列表增强）

Windows 版适用 Windows x64、Minecraft Bedrock 1.26.60.05。客户端最低要求普通 LeviLamina 26.60.8，并须完整更新其配套符号数据后重启游戏；服务端单独构建，客户端和服务端 DLL 不可混用。Android ARM64 版使用 LeviLauncher Android 的 Preloader SDK，构建和功能范围见 [Android 适配说明](<docs/android.md>)。

## 界面预览

### Windows

| 首页 | 添加服务器 | 服务器列表 |
| --- | --- | --- |
| ![Windows 主菜单：原版按钮左侧新增“服务器列表”入口](<docs/screenshots/windows-home.jpg>) | ![Windows 添加服务器表单：地址、名称与输入法提示](<docs/screenshots/windows-add-server.jpg>) | ![Windows 服务器列表：图文简介、UDP 时延与加入游戏按钮](<docs/screenshots/windows-server-list.jpg>) |

### Android

| 首页 | 添加服务器 | 服务器列表 |
| --- | --- | --- |
| ![Android 首页：社交按钮左侧的服务器入口](<docs/screenshots/android-home.jpg>) | ![Android 添加服务器表单](<docs/screenshots/android-add-server.jpg>) | ![Android 服务器列表：缩略图、时延与分页](<docs/screenshots/android-server-list.jpg>) |

## Windows 版已实现

- 主菜单独立服务器列表，支持添加、编辑、删除和选中查看详情；不读写原版列表。
- 选中服务器后显示原版绿色“加入游戏”按钮，直接连接所选地址与端口；未选中时隐藏，翻页或重开列表清除选择。
- 面板打开与关闭都是约 180 ms 的淡入淡出（smoothstep），不再整块硬切；关闭时先淡出再交还输入。
- 复用游戏原版按钮贴图和真实 Play 按钮几何，原生退出确认框出现时隐藏叠加层。
- 原创默认 64×64 草方块 Logo、960×320 风景简介图，PNG 字节编入 DLL，离线可用。
- PNG/JPEG 静态图片与 GIF 原生动画；后台 HTTP(S) 下载、受限内联 PNG、WIC 解码缩放、内容哈希磁盘缓存。
- 半透明列表/详情布局；UTF-8 光标、选区、横向滚动、复制粘贴及严格 IP/域名/端口校验。添加/编辑支持中文输入法候选词，输入过程与结束编辑后都由游戏字库绘制，保持同一字号和布局。Enter 结束编辑，Esc 取消本次字段编辑，Tab 切换字段，随后点击“保存”；支持键盘移动光标、Shift 选区与 Ctrl+A，暂不支持鼠标拖选。
- 显示服务端版本与实际 UDP 往返延迟；不以 HTTP 请求耗时冒充游戏延迟。
- 启动自动查询全部条目，打开列表补查；保存条目立即探测，手动刷新绕过新鲜度节流。
- 服务端扩展现有 NetherNet GET /v1/join，未进服即可取得 Logo、简介和简介图。
- **无需 DearOreUI、资源包或实验版 LL。** 旧网页桥接不参与默认构建。

直连使用 LL 26.60.8 发布的 `World::ServerWorldJoiner::joinExternalServer`。详见 [加入游戏与版本要求](<docs/join-game.md>)；接口申请 [#352](https://github.com/LiteLDev/mcapi-requests/issues/352) 已获发布。

## 安装与升级

- Windows 服务端正式包通过 lip 0.41 或更新版本安装，本版包绑定已验证的 LeviLamina 26.60.8。在目标服务器工作区（先停服）执行：

  ```powershell
  lip install github.com/owoser/ServerListEnhancer   # 首次安装
  lip list                                          # 应显示 26.60.8003
  lip update                                        # 后续升级到新版本
  lip pkg view github.com/owoser/ServerListEnhancer # 查看远端可安装版本
  ```

  只安装 `(side=server, platform=win-x64)` 条目；lip CLI 不管理游戏客户端。
- Windows 客户端使用 LeviLauncher 导入客户端 `.mcmod` 包（也兼容 `.zip`）；手动安装时退出游戏，把 `.mcmod` 内的全部文件解压到客户端 `mods/ServerListEnhancer/`。手动安装服务端：停止服务器，把服务端 ZIP 内同名目录放到 mods；兼容现有 plugins 目录映射。
- Android 使用 Android 版 LeviLauncher 导入 `.levipack`，不使用 Windows ZIP 或 DLL，当前不由本仓 lip 条目安装。
- **升级只替换程序和说明，保留原有服务器列表、配置和缓存；不要删除整个模组目录。** lip 条目同样保留 `mods/ServerListEnhancer/config/`、`servers.json` 与 `cache/`。
- Windows 客户端运行时会在加载初始化前拒绝 LL 26.60.8 以下版本；只替换 LL DLL 而未更新配套符号数据时，连接接口检查会提示完整升级。无需 DearOreUI。
- 配置支持 /sle reload。服务端 DLL 固定到进程生命周期，不支持热卸载或运行中替换；更新 DLL 必须重启。

## 版本与发布

版本以 [version.json](<version.json>) 为唯一事实源，全平台只有一个三段版本号，当前为 **26.60.8003**：`[lip.toml](<lip.toml>)`、各端加载器清单、Git 标签 `v26.60.8003`、GitHub Release 标题与下载包名都使用同一个值。规则为 `版本末段 = LL补丁版本 × 1000 + 自有发行序号`，自有发行序号从 1 开始、上限 999，LL 版本变更时序号从 1 重新开始；四段号会被 lip 的 SemVer 解析器拒绝，因此不再区分“显示版本”和“包版本”。

普通修复和推送代码不会自动改版本或发版；只有用户明确要求发布该版时才更新版本、创建标签和 Release。[发布准备脚本](<scripts/build-release.ps1>) 只构建和校验，不执行发布。完整的构建、草稿、资产哈希和隔离安装验证步骤见 [发布流程](<docs/releasing.md>)。

## 配置与发现协议

见 [配置和网络说明](<docs/discovery-images.md>)：字段、内联 Logo、HTTP 响应预算、TLS 策略和回退。

远程资料按实际请求的规范化 host:port 绑定，不由服务器自报 serverId 或服务器名称决定归属。编辑、删除使旧请求失效；查询失败保留旧图文，明确 enabled:false 撤销远程图文。

客户端另有 `config/config.json`，目前只有 `debugLogging` 一个开关，默认 `false`。默认只输出加载、连接与错误等低频日志；排查输入、命中或 GIF 问题时改成 `true` 再重启客户端，才会输出鼠标、点击与贴图等高频诊断日志。

## Android（ARM64）

Android 原生包使用独立 Preloader SDK，当前绑定 LeviLauncher 1.6.0。服务器入口仅在游戏首页显示，位于“社交”按钮左侧，运行时读取原版按钮资源；打开列表或离开首页即隐藏入口。列表每行自动加载 Logo 并播放 GIF，右侧显示 UDP 时延，行尾铅笔可直接编辑；软键盘避让、截图不误关、表单带闪烁光标。选中服务器后可点击绿色“加入游戏”，未选中时按钮禁用；连接沿用启动器和游戏的原生确认流程。目录、构建命令、手机导入步骤和与 Windows 的功能差异见 [Android 适配说明](<docs/android.md>)。

```powershell
./scripts/package-android.ps1
```

最终产物位于 `dist/android/`；使用 Android 版 LeviLauncher 导入 `.levipack`，不要使用 Windows DLL。

## 构建

需要 xmake、Visual Studio C++ 工具链、clang-cl。两端配置不能并发切换：

```powershell
xmake f -P . --target_type=client --ll_version=26.60.8 -y
xmake -P . -y
# 保存客户端 bin 产物后，再构建服务端
xmake f -P . --target_type=server --ll_version=26.60.8 -y
xmake -P . -y
```

每次产物位于 bin/ServerListEnhancer，切换 target 后该目录会被覆盖。默认 LL 26.60.8；客户端构建会拒绝更旧的 SDK，服务端可以独立指定版本。最低版本门禁不表示未来游戏 ABI 已验证。

默认图片为 [Logo](<resources/logo/default.png>) 和 [简介图](<resources/intro/default.png>)；使用 [生成脚本](<scripts/generate-builtin-assets.cjs>) 可重复生成原图并同步内嵌字节。

## 验证与限制

- [本轮验收记录](<docs/validation-discovery.md>)：开发服 HTTP、未进服图文、无 DearOreUI、新增探测、离线重启、图片限制、队列和配置重载。
- [图片测试](<tests/image_pipeline_test.cpp>)、[队列与持久化测试](<tests/status_queue_test.cpp>)、[原版按钮测试](<tests/vanilla_ui_test.cpp>)。
- [开发服验证脚本](<tests/server_join_live.cjs>) 仅针对明确获授权的开发服，会临时修改配置并在 finally 恢复，不要用于生产服。
- 没有可达 NetherNet HTTP 端点时不能获取实时状态，只能显示默认或旧缓存；HTTP TCP 和游戏 UDP 是不同监听协议。
- 暂不支持 WebP、SVG 或任意本地图片路径。GIF 超出动画预算时尝试静态首帧回退。中文输入通过 Windows 就地原生编辑框、Android 自有 InputConnection 接入系统输入法；地址仍按原有 ASCII 域名/IP 规则校验。
- 域名语法合法不代表一定存在或可连接；DNS/网络失败单独显示，不删除旧条目。
- [第二版规格与验收](<docs/validation-ui-gif.md>)，包含 GIF、输入校验、UDP 和大小窗口验证。
- 客户端图文只来自 `/v1/join` 的预发现结果，玩家进服后不再接收任何下发；缓存按实际请求端点隔离，不将同名缓存合并到其它服务器。

## 来源

项目原有许可声明为 MIT。默认图片为本项目原创，不含抽取的游戏贴图；原版按钮资源由游戏自身加载，不随包分发。见 [第三方说明](<THIRD_PARTY_NOTICES.md>)。
