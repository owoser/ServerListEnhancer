# MCAPI 申请草稿（ServerListEnhancer）

仓库：https://github.com/owoser/ServerListEnhancer

按 LiteLDev/mcapi-requests 已关闭 issue（如 #347）的格式整理，提交时把标题里的 #NNN 换成 issue 编号。

## 标题

Request symbols for #NNN

## Mod

ServerListEnhancer — 客户端模组，在基岩版主菜单添加完全独立的服务器列表（不读写原版 ExternalServerWorldList）。
仓库：https://github.com/owoser/ServerListEnhancer

## Platform

Client for Windows（LeviLamina 26.60.7，基岩版 1.26.60 客户端）

## Justification

第三方服务器列表（完全独立于原版 ExternalServerWorldList）需要按「地址 + 端口」直接发起连接，并且不能污染原版服务器列表：

- joinExternalServer 允许按 host/port 直接加入，是唯一不依赖原版条目的路径；
- joinExternalNetworkWorld 作为备选（需要条目 id）；
- addExternalServerWorld 仅作为最后兜底（临时插入条目拿 id，join 后立刻删除）。

当前这三个类在 26.60.7 头文件中 MCAPI 声明数为 0，全部为 MCNAPI，实测链接报 LNK2019，无法使用。

## Classes

- World::ServerWorldJoiner
- World::NetworkWorldJoiner
- World::ExternalServerWorldList

## Symbols

    ?joinExternalServer@ServerWorldJoiner@World@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@G0V?$function@$$A6AXW4JoinServerWorldResult@World@@@Z@4@@Z

    ?joinExternalNetworkWorld@NetworkWorldJoiner@World@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@V?$function@$$A6AXV?$deque@V?$unique_ptr@VProgressHandler@@U?$default_delete@VProgressHandler@@@std@@@std@@V?$allocator@V?$unique_ptr@VProgressHandler@@U?$default_delete@VProgressHandler@@@std@@@std@@@2@@std@@_N@Z@4@V?$function@$$A6AXW4JoinServerWorldResult@World@@@Z@4@@Z

    ?addExternalServerWorld@ExternalServerWorldList@World@@QEAA?AV?$expected@HW4ExternalServerWorldError@World@@@expected_lite@nonstd@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@0H@Z

## 附注

- 三个符号的修饰名均由本项目对 26.60.7 头文件编译探针 + 链接失败信息直接取得，非手工推导。
- 优先级：joinExternalServer > joinExternalNetworkWorld > addExternalServerWorld；只要第一个可用即可完成功能。
