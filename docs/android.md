# Android 原生模组（ARM64）

Android 版由 LeviLauncher Android 的 Preloader SDK 加载，使用 pl:: API，不使用 Windows LeviLamina DLL。

## 目录

- android/：独立 CMake 工程、模组清单、配置生成器、Android 平台与触控界面。android/java/ 中的自有页面视图编译为包内 ui.dex，由本模组加载，不替换启动器。
- src-client/ServerStore 与 AddressParser/TextEdit：与 Windows 共用的服务器存储、地址与 UTF-8 编辑逻辑。
- src/common/：共用发现协议、端点键和内置原创图片。
- build/android/host：主机编译器生成默认 config.json / config.schema.json 并运行协议测试。
- build/android/arm64-v8a：Android 交叉编译、依赖与 ELF 校验记录。
- dist/android/：最终 .levipack、SHA256SUMS.txt 和不含本机绝对路径的 build-info.json。

Windows xmake 的双端构建不变。Android 依赖下载及临时目录全部留在 build/，不入 Git。

## 功能与边界

通过 LeviLauncher 的 Mod Menu 启用“服务器列表增强”，在游戏首页“社交”按钮左侧点击“服务器”打开独立列表。入口是随模组安装的原版 Json UI 资源包控件（继承游戏自带 common_buttons.light_text_button，动作名写成 button.sle_home_entry，故意不被任何原版控制器处理，点它不会触发原版导航或打开浏览器），因此外观、位置与显示时机完全由游戏自身的首页界面管理，不随包分发游戏贴图。打开列表后本模组绘制整屏列表；离开首页时入口随首页一起消失，本模组界面同时关闭。支持添加、编辑、删除、分页、选中详情；触控输入框唤起软键盘；地址按与 Windows 相同的规则验证。列表保存在本模组根目录的 servers.json，不读写 Minecraft 原版列表。名称留空时与 Windows 一致：显示优先级为自定义名称、/v1/join 返回的服务器名称、域名:端口；探测结果不会回写到 servers.json，因此探测失败不会改掉已有名称。

启动器 1.6.0 的普通外部 HUD 绘图层会被首页隐藏，所以入口仍由资源包交给游戏自己绘制。实测该版本首页控制器不处理 button.menu_open_uri、button.share 与自定义动作名，JSON UI 动作无法回调到 Java，因此保留 Preloader 的原生触摸回调观察完整手势，并把按下、移动、松开全部放行给游戏，使原版按钮能显示绿色按压反馈。一次未拖动的单指点击在松开后才请求一张新鲜 PixelCopy，匹配本模组 50×24 按钮左上、右下各 2×2 逻辑像素的定位块（#C6BECA 与 #BECAC6），按连通块尺寸、配对比例与触点判定真实渲染矩形。按下和松开均在同一矩形内才打开列表；不使用固定屏幕比例、旧几何缓存或全图颜色极值，因此 GUI 标度、标度修正与安全区改变后仍按实际画面命中。探测失败、存在多个可疑配对、拖动、取消或多指手势均不打开；点击社交等原版按钮仍由游戏处理。该路径不依赖 Minecraft C++ 成员偏移，也不更改其它模块的“始终显示”设置。

列表、表单、确认页、行尾编辑与加入按钮也显示按住变绿、文字轻微下沉的反馈。松开后才执行动作，滑出或取消时清除按下状态；命中绑定控件身份和几何，翻页或界面重排后不会误触另一条目。

后台两个线程按实际 host:port 查询 /v1/join，识别 server_list_enhancer schemaVersion=1；同一端点查询失败或遇到未知扩展时保留已缓存图文，enabled:false 撤销远程图文；修改地址或端口会清空旧端点状态。请求通过独立取消监视器在超时或停用时关闭连接，覆盖等待响应头和读取正文的阶段。HTTPS 使用 Android 系统证书校验，公开元数据可按配置回退 HTTP；不禁用证书验证，不跟随重定向。

图片以 Android BitmapFactory 解码，支持 PNG、JPEG 和 GIF 的静态首帧；单文件最多 2 MiB、边长 4096、总像素 400 万，内联 PNG 解码最多 24 KiB；Logo 输出最多 512×512，简介图最多 1280×720。磁盘缓存启动时及每次写入后按 128 MiB 清理，并串行执行缓存文件替换，避免并发请求写入同一临时文件。原创默认图离线可用，包中不含游戏贴图。

以下 Windows 能力尚未移植：旧 LeviLamina 自定义 Packet，以及原生退出弹窗控件树检测。列表每行在探测完成后自动加载该服务器自己的 Logo，行内 GIF 与选中项一样按各自延时循环播放（帧图降采样到 64 px 内，最多 8 行动画，静态图与动画帧共享 32 MiB 预算并按最近使用淘汰，翻页或关闭时回收）；行尾铅笔按钮可直接编辑该行，底栏不再提供“编辑所选”。

每行右侧依次显示玩家数、UDP 延迟和版本：延迟按 41 ms / `1000+ ms` / `UDP 查询中` / `UDP 未响应` / `地址无效` 展示，≤60 ms 绿、≤100 ms 黄、更高红。探测端口优先用服务器 `/v1/join` 通告的 `server_list_enhancer.udpProbePort`，其次配置项 `udpProbePort`（0 表示直接用游戏端口）；结果缓存 15 秒，翻页或关闭列表时暂停。

列表行与资料横幅支持 GIF 动画：模组按 Windows 版同一套规则解码多帧（源文件 ≤2 MiB、帧数 ≤120、画布 ≤1024、单循环 ≤30 秒、缺省延时 100 ms、无循环扩展只播一次），逐帧注册为独立图片并按时间切换；超限或解码失败回退静态首帧。软键盘弹出时会把可用高度上报给原生侧，表单与底部按钮整体排到键盘之上。输入框获得焦点时显示闪烁光标（约 500 ms 一次），位置取 Java `Paint.measureText` 对同一字号、同一字体的实测字宽，因此紧贴最后一个字符，长文本会被夹在输入框内；取不到实测值时回退旧估算。所有按钮文案在按钮内水平居中，右侧玩家数、UDP 时延与版本三列同样按实测字宽右对齐。截图等系统浮层造成的短暂失焦不会关闭列表，真正切到后台超过约 1.2 秒才关闭。首页判定依赖画面标记，因此换成会遮蔽该控件的极高不透明度覆盖层或大幅修改 UI 的主题时，入口可能不再响应；这是刻意的保守策略，宁可点不动也不误开列表。当前适配使用按键 Unicode 输入；完整中文 IME 流程尚未接入或验证。点击条目只切换选中并展示已有图片与文案，不会重新查询服务器；资料查询由“刷新”按钮发起，另有三种自动发起：启动时、每次打开列表时（全部未在查询中的服务器）以及新增/编辑保存后的首次填充，均受配置项 autoRefresh 控制。选中条目后，列表与详情之间出现绿色“加入游戏”按钮：未选中时按钮为暗色且不可点击，选中后变亮；点击后通过启动器的 `minecraft://connect/?serverUrl=<地址>&serverPort=<端口>` 深链交给游戏本身连接，因此仍走原版确认与资源包流程。列表与表单的打开、关闭是与 Windows 一致的约 180 ms 淡入淡出；关闭时立即不再接收触摸，只是视觉上多画约 180 ms。

**Android 不做自动返回首页**：退出世界后停在原版世界列表，由玩家自己按一次返回。

原因是判定“已经停在世界列表”在 Android 上只能靠画面特征：实测这版客户端（MC 1.26.62.1 + Launcher 1.6.0）不会加载 `ui/play_screen.json` 的覆盖（用 24×24 纯品红/纯绿大标记做过 A/B，全图命中 0，而同包的 `ui/start_screen.json` 正常生效），因此只能靠采样原版按钮的像素区域来判断。玩家会使用各种资源包与主题，这些区域随时可能被改掉，误判后强行注入返回键的风险高于收益，所以这版保留原版行为，等有可靠的屏幕状态接口再考虑。

## 构建

要求 CMake ≥3.24、Ninja、Git、JDK 17、Android SDK Platform / Build Tools（android.jar 与 d8）、主机 C++20 编译器及 NDK 28.2.13676358，与 LeviLauncher Android 的构建版本一致。NDK 30 生成的库实测会因启动器 libc++ 缺少 __hash_memory 而加载失败，不能仅凭交叉编译成功判定兼容。默认仅构建 arm64-v8a、Android API 28（Android 9）及以上。

~~~powershell
./scripts/package-android.ps1
# 非默认安装位置：
./scripts/package-android.ps1 -SdkRoot 'C:/Android/Sdk' -NdkHome 'C:/Android/Sdk/ndk/28.2.13676358'
~~~

脚本优先查找 build/android-sdk/ndk/28.2.13676358，再查系统 SDK 的同版本；也接受 -NdkHome 或 ANDROID_NDK_HOME 显式指定。不会自动选取更高版本 NDK。主机默认使用 PATH 的 g++；Visual Studio 环境可用 -HostCompiler cl。缺失工具或编译失败立即终止。

完成主机构建后可运行 `./tests/Run-Android-Interaction.ps1`：执行协议与按压状态两个 CTest，以及 33 项 Java 入口回归，覆盖分数 GUI 标度、Surface 与窗口偏移、噪声标记、旧比例命中区拒绝、拖动、取消及多指手势，并核对资源包定位块和版本。2026-11-18 已通过上述测试、NDK 28.2 打包与 ELF 校验，真机确认入口来自实际渲染边界；用户随后确认安卓测试无问题。

Preloader SDK 锁定 LeviLauncher Android v1.6.0 实际绑定的提交 5d108da350ce3e7f13f51415f81e65344ecd5d14。教程模板的旧 0.2.2 结构体与 1.6.0 不兼容，实机会在 registerModule 读取回调时崩溃，因此不能用“导出函数名相同”证明 C++ ABI 兼容。采用官方模板默认的 Preloader 链接方式：从锁定源码构建运行库作为链接输入，让本模组记录 libpreloader.so 的 DT_NEEDED 依赖，并在链接阶段检查 API 符号。安装包仍只包含本模组的 .so，不分发、替换启动器的 libpreloader.so 或 libc++_shared.so。构建会检查 ARM64 ELF、PLGetModRegistration 导出、16 KiB LOAD 对齐、动态依赖和已知不兼容的 C++ 符号；仍需实机验证加载。

## 安装与验证

1. 将 dist/android/ServerListEnhancer-1.0.0-arm64-v8a.levipack 传到手机下载目录。
2. 在 LeviLauncher Android 当前 Minecraft 实例的“模组”页面添加该文件，启用模组。
3. 清单声明 Minecraft 1.26.60 与 1.26.62 系列；在匹配实例启动后，从 Mod Menu 检查模块，返回游戏首页检查“社交”左侧的“服务器”按钮。打开列表、进入设置/游戏列表或世界内时入口应消失。
4. 使用 adb logcat -s ServerListEnhancer 查看加载日志；添加一条测试服务器并重启检查列表保留。

包的唯一顶层目录是 server_list_enhancer，包含 manifest.json、libserver_list_enhancer.so、ui.dex、config/ 和资源/说明。不要把 Windows DLL 改扩展名后导入；更新时保留现有 servers.json 与用户配置。本模组注册的输入回调无法注销，因此将自身固定到游戏进程生命周期，更新和彻底禁用请退出游戏。当前构建仅绑定并验证启动器 1.6.0，其他版本应先核对 SDK 结构体 ABI。

## 官方资料

- [第一个 Android 原生模组](https://levimc.org/zh-Hans/tutorials/develop/first-android-mod/)
- [教程指定模板](https://github.com/QYCottage/levilauncher-android-mod-template)
- [Preloader Android SDK](https://github.com/LiteLDev/preloader-android/tree/5d108da350ce3e7f13f51415f81e65344ecd5d14)
- [官方完整 C++ 示例](https://github.com/LiteLDev/LeviLauncher-Android/tree/main/examples/full-cpp-mod)
