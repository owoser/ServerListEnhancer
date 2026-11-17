# ServerListEnhancer（服务器列表增强）

适用：Windows x64、Minecraft Bedrock 1.26.60.05、普通 LeviLamina 26.60.7。客户端和服务端 DLL 不可混用。

## 已实现

- 主菜单独立服务器列表，支持添加、编辑、删除和选中查看详情；不读写原版列表。
- 复用游戏原版按钮贴图和真实 Play 按钮几何，原生退出确认框出现时隐藏叠加层。
- 原创默认 64×64 草方块 Logo、960×320 风景简介图，PNG 字节编入 DLL，离线可用。
- PNG/JPEG 静态图片与 GIF 原生动画；后台 HTTP(S) 下载、受限内联 PNG、WIC 解码缩放、内容哈希磁盘缓存。
- 半透明列表/详情布局；UTF-8 光标、选区、横向滚动、复制粘贴及严格 IP/域名/端口校验。
- 显示服务端版本与实际 UDP 往返延迟；不以 HTTP 请求耗时冒充游戏延迟。
- 启动自动查询全部条目，打开列表补查；保存条目立即探测，手动刷新绕过新鲜度节流。
- 服务端扩展现有 NetherNet GET /v1/join，未进服即可取得 Logo、简介和简介图。
- **无需 DearOreUI、资源包或实验版 LL。** 旧网页桥接不参与默认构建。

> 点击直连仍受当前客户端 MCAPI 导出限制，本轮没有解决；点击条目现在选择详情。见 [接口申请说明](<docs/mcapi-request.md>)。

## 安装与升级

- 客户端：退出游戏，把客户端包内的 ServerListEnhancer 目录放到客户端 mods 目录。
- 服务端：停止服务器，把服务端包内同名目录放到 mods；兼容现有 plugins 目录映射。
- **升级只替换程序和说明，保留原有服务器列表、配置和缓存；不要删除整个模组目录。**
- 客户端仍需 LeviLamina 客户端版，但不需要 DearOreUI。
- 配置支持 /sle reload。服务端 DLL 固定到进程生命周期，不支持热卸载或运行中替换；更新 DLL 必须重启。

## 配置与发现协议

见 [配置和网络说明](<docs/discovery-images.md>)：字段、内联 Logo、HTTP 响应预算、TLS 策略和回退。

远程资料按实际请求的规范化 host:port 绑定，不由服务器自报 serverId 或服务器名称决定归属。编辑、删除使旧请求失效；查询失败保留旧图文，明确 enabled:false 撤销远程图文。

## 构建

需要 xmake、Visual Studio C++ 工具链、clang-cl。两端配置不能并发切换：

```powershell
xmake f --target_type=client -y
xmake -y
# 保存客户端 bin 产物后，再构建服务端
xmake f --target_type=server -y
xmake -y
```

每次产物位于 bin/ServerListEnhancer，切换 target 后该目录会被覆盖。默认 LL 26.60.7，其它版本需要重新验证。

默认图片为 [Logo](<resources/logo/default.png>) 和 [简介图](<resources/intro/default.png>)；使用 [生成脚本](<scripts/generate-builtin-assets.cjs>) 可重复生成原图并同步内嵌字节。

## 验证与限制

- [本轮验收记录](<docs/validation-discovery.md>)：开发服 HTTP、未进服图文、无 DearOreUI、新增探测、离线重启、图片限制、队列和配置重载。
- [图片测试](<tests/image_pipeline_test.cpp>)、[队列与持久化测试](<tests/status_queue_test.cpp>)、[原版按钮测试](<tests/vanilla_ui_test.cpp>)。
- [开发服验证脚本](<tests/server_join_live.cjs>) 仅针对明确获授权的开发服，会临时修改配置并在 finally 恢复，不要用于生产服。
- 没有可达 NetherNet HTTP 端点时不能获取实时状态，只能显示默认或旧缓存；HTTP TCP 和游戏 UDP 是不同监听协议。
- 暂不支持 WebP、SVG 或任意本地图片路径。GIF 超出动画预算时尝试静态首帧回退；输入框可粘贴中文，但未实现完整 IME 组合输入。
- 域名语法合法不代表一定存在或可连接；DNS/网络失败单独显示，不删除旧条目。
- [第二版规格与验收](<docs/validation-ui-gif.md>)，包含 GIF、输入校验、UDP 和大小窗口验证。
- 旧可选进服 Packet 的接收/缓存保留；新预发现使用独立端点缓存，不将同名旧缓存猜测合并到其它服务器。

## 来源

项目原有许可声明为 MIT。默认图片为本项目原创，不含抽取的游戏贴图；原版按钮资源由游戏自身加载，不随包分发。见 [第三方说明](<THIRD_PARTY_NOTICES.md>)。
