# 任务05 RK3568 Web、SQLite与Modbus TCP状态上报

**2026-11-24阶段收尾：Modbus TCP已在真实RK3568和局域网上跑通，启停管理及30分钟三客户端测试通过。RTU按用户要求暂缓，不作为本阶段收尾阻塞。**

- [阶段总结与完成边界](docs/stage-summary.md)
- [架构与数据所有权](docs/architecture.md)
- [Modbus TCP点表与主站接入](docs/protocol.md)
- [构建、运行与排错](docs/reproduction.md)
- [验收矩阵与30分钟结果](docs/acceptance.md)
- [公开证据说明](evidence/README.md)、[第三方依赖](third_party/README.md)

## 已完成内容

主要应用使用C：Linux网络状态采集、libmodbus TCP只读服务、C只读主站、SQLite历史/配置库、C SCGI接口。静态网页展示实时状态与历史，私有管理页区分已保存配置和实际运行配置。Python提供测试及独立TCP启停工具。

| 验证层次 | 实测结果 |
| --- | --- |
| Web/SQLite/配置主机回归 | 124项通过，AArch64构建成功 |
| TCP专项 | 57项主机检查；板端C主站8次读取；Windows局域网19项检查通过 |
| 运行管理 | 14项主机管理+3项测试工具检查；板端9项启停检查通过 |
| 30分钟TCP | 3客户端各1Hz，5400/5400成功，零错误、零重连；p95 222.31ms |

上述测试集合有复用，不能简单相加当作独立测试总数。30分钟仅TCP负载，不是Web/SQLite/RTU联合压测。

## 当前边界

实际环境为ATK-DLRK3568、AArch64、Linux4.19.232、Buildroot2018.02-rc3；Ubuntu22.04.5 x86_64交叉开发。使用Wi-Fi，PuTTY为调试控制台，不充当RTU通信口。原需求中的i.MX6ULL/Cortex-A7/10/100M假设不沿用。

程序已临时部署到真实开发板并实测；结束后任务服务已停止，502释放。持久化程序部署、常驻、开机自启不是证明TCP跑通的必要条件，本阶段不再自动推进。实体RS485、IP变更回退、板卡重启/掉电及更长联合测试仍保留。

## 仓库内容

src/为C源码，web/为页面，scripts/为构建与管理工具，tests/为可迁移的主机测试，evidence/为精选脱敏实测记录。源码不包含SDK、运行数据库、实际管理令牌、SSH配置、私钥或目标二进制。

C源码与已验收版本逐文件一致。公开构建脚本改为显式依赖路径，合成测试IPv4改用文档地址；这些公开路径适配仅做静态检查，没有重新构建/部署。历史验收以原环境版本为准。Ubuntu复现命令集中在commands.txt，均不表示本轮执行。
