# Android 原生模组（ARM64）

Android 版由 LeviLauncher Android 的 Preloader SDK 加载，使用 pl:: API，不使用 Windows LeviLamina DLL。

## 目录

- android/：独立 CMake 工程、模组清单、配置生成器、Android 平台与触控界面。
- src-client/ServerStore 与 AddressParser/TextEdit：与 Windows 共用的服务器存储、地址与 UTF-8 编辑逻辑。
- src/common/：共用发现协议、端点键和内置原创图片。
- build/android/host：主机编译器生成默认 config.json / config.schema.json 并运行协议测试。
- build/android/arm64-v8a：Android 交叉编译、依赖与 ELF 校验记录。
- dist/android/：最终 .levipack、SHA256SUMS.txt 和不含本机绝对路径的 build-info.json。

Windows xmake 的双端构建不变。Android 依赖下载及临时目录全部留在 build/，不入 Git。

## 功能与边界

通过 LeviLauncher 的 Mod Menu 启用“服务器列表增强”，进入世界的 HUD 后，点击“服务器”浮动按钮打开独立列表。启动器 1.6.0 会在主菜单隐藏浮动按钮和绘图层，因此当前 Android 版尚不能在主菜单管理服务器。支持添加、编辑、删除、分页、选中详情；触控输入框唤起软键盘；地址按与 Windows 相同的规则验证。列表保存在本模组根目录的 servers.json，不读写 Minecraft 原版列表。

后台两个线程按实际 host:port 查询 /v1/join，识别 server_list_enhancer schemaVersion=1；同一端点查询失败或遇到未知扩展时保留已缓存图文，enabled:false 撤销远程图文；修改地址或端口会清空旧端点状态。请求通过独立取消监视器在超时或停用时关闭连接，覆盖等待响应头和读取正文的阶段。HTTPS 使用 Android 系统证书校验，公开元数据可按配置回退 HTTP；不禁用证书验证，不跟随重定向。

图片以 Android BitmapFactory 解码，支持 PNG、JPEG 和 GIF 的静态首帧；单文件最多 2 MiB、边长 4096、总像素 400 万，内联 PNG 解码最多 24 KiB；Logo 输出最多 512×512，简介图最多 1280×720。磁盘缓存启动时及每次写入后按 128 MiB 清理，并串行执行缓存文件替换，避免并发请求写入同一临时文件。原创默认图离线可用，包中不含游戏贴图。

这是 Android 平台首次适配，以下 Windows 能力尚未移植：GIF 连续播放、UDP RTT、旧 LeviLamina 自定义 Packet、仅主菜单出现的入口及原生退出弹窗联动。当前适配使用按键 Unicode 输入；虽然匹配的 SDK 还提供文本提交回调，完整中文 IME 流程尚未接入或验证。点击条目选择详情；直连没有实现。使用后请点击关闭，避免列表覆盖游戏。

## 构建

要求 CMake ≥3.24、Ninja、Git、主机 C++20 编译器及 NDK 28.2.13676358，与 LeviLauncher Android 的构建版本一致。NDK 30 生成的库实测会因启动器 libc++ 缺少 __hash_memory 而加载失败，不能仅凭交叉编译成功判定兼容。默认仅构建 arm64-v8a、Android API 28（Android 9）及以上。

~~~powershell
./scripts/package-android.ps1
# 非默认安装位置：
./scripts/package-android.ps1 -SdkRoot 'C:/Android/Sdk' -NdkHome 'C:/Android/Sdk/ndk/28.2.13676358'
~~~

脚本优先查找 build/android-sdk/ndk/28.2.13676358，再查系统 SDK 的同版本；也接受 -NdkHome 或 ANDROID_NDK_HOME 显式指定。不会自动选取更高版本 NDK。主机默认使用 PATH 的 g++；Visual Studio 环境可用 -HostCompiler cl。缺失工具或编译失败立即终止。

Preloader SDK 锁定 LeviLauncher Android v1.6.0 实际绑定的提交 5d108da350ce3e7f13f51415f81e65344ecd5d14。教程模板的旧 0.2.2 结构体与 1.6.0 不兼容，实机会在 registerModule 读取回调时崩溃，因此不能用“导出函数名相同”证明 C++ ABI 兼容。采用官方模板默认的 Preloader 链接方式：从锁定源码构建运行库作为链接输入，让本模组记录 libpreloader.so 的 DT_NEEDED 依赖，并在链接阶段检查 API 符号。安装包仍只包含本模组的 .so，不分发、替换启动器的 libpreloader.so 或 libc++_shared.so。构建会检查 ARM64 ELF、PLGetModRegistration 导出、16 KiB LOAD 对齐、动态依赖和已知不兼容的 C++ 符号；仍需实机验证加载。

## 安装与验证

1. 将 dist/android/ServerListEnhancer-1.0.0-arm64-v8a.levipack 传到手机下载目录。
2. 在 LeviLauncher Android 当前 Minecraft 实例的“模组”页面添加该文件，启用模组。
3. 清单声明 Minecraft 1.26.60 与 1.26.62 系列；在匹配实例启动后，从 Mod Menu 检查模块，再进入测试世界的 HUD 检查“服务器”按钮。
4. 使用 adb logcat -s ServerListEnhancer 查看加载日志；添加一条测试服务器并重启检查列表保留。

包的唯一顶层目录是 server_list_enhancer，包含 manifest.json、libserver_list_enhancer.so、config/ 和资源/说明。不要把 Windows DLL 改扩展名后导入；更新时保留现有 servers.json 与用户配置。本模组注册的输入回调无法注销，因此将自身固定到游戏进程生命周期，更新和彻底禁用请退出游戏。当前构建仅绑定并验证启动器 1.6.0，其他版本应先核对 SDK 结构体 ABI。

## 官方资料

- [第一个 Android 原生模组](https://levimc.org/zh-Hans/tutorials/develop/first-android-mod/)
- [教程指定模板](https://github.com/QYCottage/levilauncher-android-mod-template)
- [Preloader Android SDK](https://github.com/LiteLDev/preloader-android/tree/5d108da350ce3e7f13f51415f81e65344ecd5d14)
- [官方完整 C++ 示例](https://github.com/LiteLDev/LeviLauncher-Android/tree/main/examples/full-cpp-mod)
