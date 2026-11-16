# 进服前发现与图片配置

## 服务端配置

配置位于 mods/ServerListEnhancer/config/config.json（旧环境可能通过 plugins 映射访问）。首次启动生成默认值，升级保留原配置。示例：

```json
{
  "version": 1,
  "enabled": true,
  "serverId": "my-server",
  "logoId": "default",
  "logoUrl": "",
  "logoData": "",
  "introText": "欢迎来到本服务器\n第二行简介",
  "introImageId": "default",
  "introImageUrl": "",
  "logLevel": 4
}
```

- Logo 优先级：合法非空 logoData > logoUrl > 内置 default。
- logoData 仅接受规范 data:image/png;base64,...，解码数据 ≤24 KiB；仅用于 HTTP，不改变旧二进制 Packet。删除该键或置空后重载会取消覆盖。
- 简介图使用 introImageUrl，为空使用内置风景图，大图不要放进状态 JSON。
- URL 仅 http/https，服务端字段最多512字节；简介最多1024 UTF-8字节；ID仅字母、数字、下划线、连字符，最多64字节。
- 当前客户端真正内置的 ID 是 default。未知 ID 安全回退 default，不能用 ID 传任意路径。
- serverId 用于诊断，不是客户端缓存归属，建议使用英文稳定 ID。
- 修改后执行 /sle reload，发布不可变 HTTP 快照，并兼容推送给在线客户端。/sle push 仅重新推送旧 Packet。
- enabled:false 显式撤销远程图文，不破坏原生状态响应。
- **不支持服务端热卸载。配置重载不等于 DLL 升级，更新必须停止并重启服务器。**

## HTTP 响应

复用原生 NetherNet GET /v1/join，不另开 SLE 专用服务。保留正常名称、人数、游戏版本、协议等原生字段，追加：

```json
{
  "server_list_enhancer": {
    "schemaVersion": 1,
    "revision": "内容变更标识",
    "enabled": true,
    "cacheTtlSeconds": 300,
    "serverId": "my-server",
    "logo": {"type":"builtin", "value":"default"},
    "introText":"欢迎来到本服务器",
    "introImage": {"type":"url", "value":"https://example.com/banner.png"}
  }
}
```

客户端只认 schemaVersion=1；未知/错误扩展不破坏基础状态。Logo 支持 builtin/url/data，简介图建议 builtin/url。revision 是变更标识，不是签名或身份认证。

```powershell
curl.exe --noproxy "*" http://127.0.0.1:19132/v1/join
```

前提是原生 HTTP 信令监听存在并且可达。游戏 UDP 可达不等于 HTTP TCP 可达；远程玩家不能使用自己的127.0.0.1访问你的服务器。纯 RakNet 或没有 HTTP 的服务器显示默认或旧缓存。

## 客户端行为

- 自有服务器条目独立管理，不读写原版列表，手动配置图文优先于发现资料。
- 启动自动排队查全部条目，4个状态工作线程，超过4台排队不丢弃。打开列表按新鲜度补查；添加/编辑保存立即探测；手动刷新绕过新鲜度并合并同端点在途请求。
- 状态通常60秒节流，视觉TTL默认300秒；请求总预算4秒，完整JSON最多64 KiB，分块响应亦在追加前检查。
- 实际端点和请求代际决定归属，同名或相同serverId不互相覆盖；编辑/删除后拒绝旧结果。
- cache/discovery.json 保存预发现资料；cache/images 保存规范PNG及引用映射。已有图片立即复用，不等网络刷新结束；下次启动可重建被删除的默认图片缓存。
- 失败保留旧图文并标记失败；明确enabled:false清除远程图文，无历史图片立即用内置默认图。

## 图片预算

PNG/JPEG单帧；输入≤2 MiB、边长≤4096、总像素≤4,000,000；内联PNG解码≤24 KiB。WIC先检查尺寸再分配像素，后台缩放为规范PNG：Logo≤512×512，简介图≤1600×900，保持比例。

图片2个工作线程，总预算10秒；失败冷却5分钟；磁盘预算128 MiB，保护默认图片、淘汰旧哈希文件。渲染回调不下载、不解码。外部不能指定本地路径。

仅HTTP(S)，拒绝凭据和重定向，HTTPS严格验证证书；不支持SVG、WebP或动画。本轮未实现GPU纹理总量预算，也不承诺释放游戏自身的纹理缓存。

## TLS边界

当前客户端显式启用旧NetherNet元数据的自签证书兼容策略，并在特定连接/信道失败时回落同端点HTTP；**这不提供服务器身份认证**。状态资料应视为不可信输入，不承载秘密。不要将该策略用于其它HTTPS业务。

图片下载独立使用严格证书策略，不沿用元数据的证书忽略设置。明文HTTP不防中间人；重要部署建议使用可信TLS端点。
