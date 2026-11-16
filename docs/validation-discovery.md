# 进服前图文与图片管线验收

日期：2026-11-16。实际环境：Minecraft Bedrock 1.26.60.05 / LeviLamina 26.60.7、Windows x64。开发服使用用户授权的 D:/MCserver/SER-DEV。未使用实验LL替换普通LL。

## 已通过

| 项目 | 结果与证据 |
| --- | --- |
| 两端构建 | xmake client/server 分开配置和构建均成功 |
| 无DearOreUI | 将原前置移出mods后启动，客户端正常加载，原生按钮及列表可用 |
| 首次启动自动探测 | 打开列表前，discovery.json已出现本次启动请求时间戳和服务端扩展 |
| 未进服图文 | 实际/v1/join得到Logo、三行简介和简介图片；客户端显示真实远程JPEG，lastJoinedAt仍为0 |
| 添加后自动探测 | 真实表单添加此前未使用的localhost:19132，未加入游戏，列表立即得到相同服务端图文 |
| 原生本地纹理 | Raw ResourceLocation从本地规范PNG加载并drawImage，实机可见远程Logo/简介图与内置图 |
| 离线重启 | 停开发服后重启最终客户端，默认条目显示原创草方块/风景，已发现条目保留Logo、三行简介和缓存图片 |
| 小窗口 | 实际1264×721窗口，列表、详情图、文字、按钮布局均可见 |
| 退出框 | 最终客户端原生退出确认框出现，整层停止绘制，无服务器列表按钮遮挡 |
| 状态队列 | 12项任务/4工作线程，不丢队列，force绕新鲜度，删除后晚到结果不复活 |
| 状态缓存 | 持久化加载后立即可读；离线保留资料；enabled:false撤销；未知schema不破坏基础状态 |
| 图片安全 | PNG/JPEG解码/规范化；4097边长、2048²总像素、>2MiB、坏/截断PNG、非规范或过大base64、非法URL均拒绝 |
| 内联图 | 后台内联PNG解码、规范缓存、重新初始化后复用通过 |
| HTTP边界 | 64KiB追加前限制；实际Content-Length等于UTF-8响应字节数；原生字段均保留 |
| 服务端重载 | /sle reload执行；enabled:false、内联PNG、revision变化、删除logoData恢复URL、20并发GET全部通过，最后配置逐字节恢复 |

## 实机图片（工作区build目录）

- discovery-list-real.png：未进服，实际远程Logo/三行简介/简介图。
- discovery-added-ready.png：新增localhost条目自动显示。
- final-offline-list.png：最终客户端内置草方块与风景图。
- final-offline-cache.png：服务端停止、客户端重启后保留远程图文。
- final-small-images.png：小窗口原生渲染。
- final-modal-small.png：最终退出弹窗无叠加。

## 回归执行

源码测试：tests/image_pipeline_test.cpp、tests/status_queue_test.cpp、tests/vanilla_ui_test.cpp。前两者用clang-cl C++20、EHsc、utf-8构建；图片链接windowscodecs/ole32/bcrypt/winhttp，状态链接winhttp及nlohmann_json头。状态测试的最后一项需要开发服正在运行。

开发服实测：node tests/run-dev-server.cjs（受管理后台），node tests/server_join_live.cjs。后者临时修改开发服配置，并在finally恢复；仅在明确授权环境执行。

发现并修复了原/sle子命令未声明required参数，以及可选logoData删除后重载未清空的问题。服务器生成的对象成员偏移在实机不可靠，最终实现改为导出onRequest边界的类型化异步响应装饰，不再访问HttpSignalingServer内部callback布局。

## 最终产物身份

- 客户端DLL：972288字节，SHA-256 8A6D26FD472EF7E6A72C06C3E24C904C5EDC512E6237052DE5CDA1E7D855340C。
- 服务端DLL：454144字节，SHA-256 428C24332CB1187BDDE1842633C932F5A03E2D4E23A4FDABACC0069FE524B3B6。
- 两者与实际部署路径哈希一致。

## 明确未完成的历史能力

点击直连仍受普通LL26.60.7客户端MCAPI导出限制；本轮不将它冒充已实现。完整IME、WebP/动图、GPU纹理缓存预算也不在本轮完成范围。服务端支持配置重载，但DLL固定进程生命周期，不支持热卸载。
