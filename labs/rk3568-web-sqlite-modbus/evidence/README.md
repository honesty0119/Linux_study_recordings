# 公开验收证据

## 来源与脱敏

所有数值来自本任务真实执行结果。公开文本将个人路径改为TASK_WORKSPACE、板端地址改为BOARD_IPV4、临时根改为BOARD_*_ROOT；不包含凭据、数据库或原始连接配置。主机测试中的回环地址/合成输入不是用户网络配置。

- tcp-build.log、ops-check.log：已执行构建/工具验证标记。
- host-*.json：对应主机协议、C主站、管理器和测试工具结果。
- config-board.json：配置保存/实际加载状态快照；scope保留原机器记录当时的manual browser pending，后续用户确认见文档，未改写原记录冒充程序验收。
- board-manager.json：板端9项短程启停检查。
- tcp-30min-summary.json：完整1800秒、5400读结果；配置地址/路径脱敏，不改变统计。
- tcp-30min-client-*.jsonl：每客户端首5+末5条，**不是完整5400行**；选择说明和原始完整文件SHA256见sample-selection.json。
- tcp-30min-resources.jsonl及summary：完整180次资源记录与统计，保留末次RSS变化。
- tcp-30min-server.log：三个连接各1800请求及正常停止，监听地址脱敏。

完整原始日志在授权任务目录保留；公开原始哈希用于追溯，不把摘要当作第三方独立认证。
