# 版本与正式发布

本流程只在用户明确要求发布某一版本时执行。普通修复、测试、构建、提交和“推送代码”均不授权修改版本、创建或推送版本标签、创建 Release 草稿或公开发版。一次授权只覆盖当次版本；不得沿用以前的授权发布下一版，也不配置每次 push 自动发版。

## 版本唯一事实源

根目录 [version.json](<../version.json>) 保存三个字段，构建与打包均从这里取值。修改版本属于发版动作，未获该次明确授权时不得自动 bump。

| 字段 | 含义 | 本次示例 |
| --- | --- | --- |
| `version` | 全平台唯一三段版本号，同时用于 lip、加载器清单、标签、Release 标题和包名 | `26.60.8001` |
| `leviLaminaVersion` | Windows 已验证的 LL 构建版本 | `26.60.8` |
| `revision` | 对应 LL 版本下的自有发行序号，从 1 开始，范围 1..999 | `1` |

设 LL 版本为 `a.b.c`，自有序号为 `r`，则版本号为 `a.b.(c×1000+r)`；本轮 `26.60.8` + `1` → `26.60.8001`。LL 版本变更时序号从 `1` 重新开始；序号达到 999 后不得自行溢出，需要重新确认版本方案。lip 0.41.1 实测拒绝四段 SemVer，`26.60.8.1` 这类四段号禁止出现在 `lip.toml`、加载器清单或标签里。

以下身份全部使用同一个版本号，必须一致：

- [lip.toml](<../lip.toml>) 的 `version`、Windows 与 Android 模组加载器的 `manifest.version` 均为 `26.60.8001`。
- Git 版本标签为 `v<version>`，本次为 **`v26.60.8001`**。标签所指提交中的 lip 版本必须与标签严格一致，否则 lip 会跳过该标签。
- GitHub Release 标题为版本号 **`26.60.8001`**。
- Windows 下载包名为 `ServerListEnhancer-client-v26.60.8001.zip` 和 `ServerListEnhancer-server-v26.60.8001.zip`；Android 下载包名为 `ServerListEnhancer-26.60.8001-arm64-v8a.levipack`。
- 下载 URL 的标签段与资产文件名使用同一个版本号，例如 `releases/download/v26.60.8001/ServerListEnhancer-server-v26.60.8001.zip`。

LL 最低运行门禁、lip 依赖、构建 SDK 与游戏 ABI 是不同检查项。本次 Windows lip 依赖精确绑定 LL `26.60.8`；最低门禁通过不证明未来 LL/游戏版本已验证。Android 使用独立的 Preloader SDK，继续遵守 [Android 构建说明](<android.md>) 的启动器、NDK 与游戏版本约束。

## lip 安装模型

包 ID 固定为 `github.com/owoser/ServerListEnhancer`。清单采用 format 4；默认变体 `""` 包含两个 Windows 条目，分别是 `(side=server, platform=win-x64)` 与 `(side=client, platform=win-x64)`。

Windows ZIP 根部包含 `ServerListEnhancer/`，因此清单使用 `type="zip"`，以 `{ src="ServerListEnhancer/", dest="mods/ServerListEnhancer/" }` 放置。不要改成要求 ZIP 根部直接有模组清单的 `mcmod` 资源类型。`preserve` 按工作区相对路径匹配，必须保留：

```text
mods/ServerListEnhancer/config/
mods/ServerListEnhancer/servers.json
mods/ServerListEnhancer/cache/
```

lip CLI 仅管理服务端工作区，安装命令为 `lip install github.com/owoser/ServerListEnhancer`；Windows 客户端条目由 LeviLauncher 消费。Android 通过 Android 版 LeviLauncher 导入 `.levipack`，本仓的 Windows lip 条目不安装 Android。

## 发布前：准备并校验产物

1. 核对本次用户发版指令、目标版本号和兼容范围。按批准值更新唯一版本源，并让 lip 清单、加载器清单及打包结果保持一致。不要根据普通修复或 push 自行分配新版本。
2. 在仓库根确认 lip 为 0.41 或更新版本，使用真实解析器校验清单及模板展开：

   ```powershell
   lip -v
   lip pkg view .
   ./scripts/build-release.ps1 -ValidateOnly
   ```

   检查 `manifest.version`、两端 `side/platform`、LL 依赖及完整下载 URL。`-ValidateOnly` 不执行构建。
3. 运行 [发布准备脚本](<../scripts/build-release.ps1>)：

   ```powershell
   ./scripts/build-release.ps1
   ```

   脚本只读取版本、构建和校验，不改版本、不建标签、不 push、不创建或发布 Release。Windows 必须串行构建 client/server；每端产物先从 `bin/ServerListEnhancer` 保存到各自暂存目录，再切下一端，不能并发切 xmake 配置。两端共用 `sle.modpacker`，不能依靠遗留暂存 DLL 冒充本次构建。Android 随后执行独立打包和主机 CTest、DEX、ARM64 ELF/依赖检查。
4. 按本轮改动运行必要回归，记录实际结果。中文输入要切到中文模式，走拼音、候选选词和提交；英语布局下的主机字符测试不能代替 IME 验证。需联网或启动游戏时只用已授权的开发环境，不把生产服或玩家资料用作测试。
5. 打开每个最终归档核对清单：`version=<version>`，Windows `platform` 与 client/server 相符，构建 LL 版本与 `leviLaminaVersion` 相符；依赖与已验证范围一致。确认 ZIP 内目录形状匹配 `place`、Android 包内包含原生库及 DEX、第三方说明齐备；不得混入用户列表、缓存或本机配置。
6. 检查 [dist](<../dist/>) 中三类正式包及 `SHA256SUMS.txt`。摘要中的三个文件名和 SHA256 必须对应本次最终产物，不使用旧版本文件或构建前摘要。
7. 所有受管理源码、脚本和文档达到最终稳定状态后，由集成 lead 按当前 AOCI Guide 完成普通维护，处理全部批次与停点，再依次证明对齐：

   ```powershell
   & C:/aoci/bin/aoci.exe --repo . verify --json
   & C:/aoci/bin/aoci.exe --repo . check --json
   & C:/aoci/bin/aoci.exe --repo . index agent guide --agent lead --json
   ```

   以当前工具事实为准，要求 Guide `complete=true`、`next_action=none`，且无待处理漂移、Scope 审阅或 Recovery。不得把中间状态的维护结果用于最后发版；随后又修改受管内容时须重新收尾。
8. 审阅最终差异，提交并 push 本轮源码、版本、文档和正式索引；确认将发布的提交已在远端，记录完整提交 SHA。最终包应来自该提交对应的业务源码，不使用随后漂移的工作区产物。

## 已获本次发版授权后：草稿、上传与公开

先确认 GitHub CLI 已登录正确账号及仓库权限。以下操作属于发布，不能由构建脚本或普通 push 自动触发。已有同名 tag/Release 时先检查它们的目标提交、草稿状态和资产；不要盲目重复创建，不移动已公开标签，也不覆盖已公开版本的资产。

在仓库根从唯一版本源读取值，固定发布目标提交，并准备 UTF-8 发布说明文件：

```powershell
$release = Get-Content ./version.json -Raw | ConvertFrom-Json
$tag = "v$($release.version)"
$commit = git rev-parse HEAD
# 先核对 $commit 已推送，且 build/release-notes.md 是本版已审阅的 UTF-8 发布说明。
gh release create $tag --repo owoser/ServerListEnhancer --draft --target $commit --title $release.version --notes-file build/release-notes.md
```

草稿创建成功后，上传三个正式包与本地摘要：

```powershell
gh release upload $tag --repo owoser/ServerListEnhancer "dist/ServerListEnhancer-client-v$($release.version).zip" "dist/ServerListEnhancer-server-v$($release.version).zip" "dist/android/ServerListEnhancer-$($release.version)-arm64-v8a.levipack" dist/SHA256SUMS.txt
```

每条命令非零退出或结果未知时先查看该草稿及其资产，不盲目重试，不继续公开。通过已登录账号把草稿资产下载到本次新建的 [build](<../build/>) 隔离目录；核对资产数目、文件名、大小和每个包的 SHA256，下载的摘要文件也须与本地最终摘要一致：

```powershell
$verifyDir = "build/release-assets-$([guid]::NewGuid().ToString('N'))"
gh release download $tag --repo owoser/ServerListEnhancer --dir $verifyDir
# 逐项核对下载包和 SHA256SUMS.txt；全部相符后才执行下一行。
gh release edit $tag --repo owoser/ServerListEnhancer --draft=false
```

公开后再次核对 tag 指向记录的提交，Release 标题就是版本号，资产可公开下载。保存 Release URL、tag、提交 SHA 和最终三包摘要。如果公开后发现问题，先报告具体版本与资产；不要静默移动标签或覆盖原有资产，修复版需要该次新的明确发布指令。

## 公开后的远端与隔离安装验证

```powershell
lip pkg view github.com/owoser/ServerListEnhancer
```

确认远端可发现 `26.60.8001` 等目标包版本，且其清单和展开后的下载 URL 指向目标 tag 与资产；不能只凭 GitHub Release 存在就声称 lip 可安装。

安装验证只在本次新建的空隔离目录进行，目录放在仓库 [build](<../build/>) 下并使用唯一名称。空目录还没有游戏，而本包会连带安装 LeviLamina 26.60.8，lip 会拒绝并提示先准备服务端（`error: This workspace has no game yet ...`）；因此先在同一目录执行 `lip install github.com/LiteLDev/LeviLamina --accept-eula`。接受 Minecraft EULA 与隐私政策只能由维护者本人决定，agent 不得代为加 `--accept-eula`。随后运行 `lip install github.com/owoser/ServerListEnhancer` 和 `lip list`，确认安装版本、LL 依赖以及 `mods/ServerListEnhancer/` 中的 DLL/清单。可在隔离目录预置临时 `config/`、`servers.json`、`cache/` 哨兵文件并记录哈希，核对安装或更新后保持不变。不要复制真实服务器或玩家资料，不切换到真实游戏/服务器目录安装，也不为测试而删除其模组目录。

安装完成与实机启动是不同证据；如需启动验证，只运行本次明确授权的隔离/开发实例。Windows 客户端通过 LeviLauncher 验证客户端条目，Android 走 `.levipack` 导入与日志验证，不把服务端 lip 安装结果外推到另外两端。发布完成后汇报实际通过的检查、未验证项和三个下载入口。

## 参考

以下官方文档于 2026-11-19 核对，本项目沿用其清单和发现规则；构建采用本地准备加人工控制的草稿发布，不照搬教程的自动构建发布触发器。

- [用 lip 发布你的模组](https://levimc.org/zh-Hans/tutorials/develop/publish-a-package/)
- [lip.toml 格式 4、变体、资源与保留规则](https://levimc.org/zh-Hans/lip/concepts/lip_toml.html)
- [项目规则](<../AGENTS.md>) 与 [安装说明](<../README.md>)
