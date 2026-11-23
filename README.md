# Linux 学习记录

## 任务03：RK3568 Qt串口助手交付（2026-11-23）

主要功能已在LCD与OTG链路实测，含双向二进制、文件/日志、缓存边界和热拔插恢复。仍有实体UART参数、权限失败、持续负载等保留项。

- [任务03源码与交付入口](labs/rk3568-qt-serial-assistant/README.md)
- [图文验收报告（9张调试原图）](labs/rk3568-qt-serial-assistant/docs/acceptance-report.md)
- [Release交付包](https://github.com/honesty0119/Linux_study_recordings/releases/tag/task03-qt-serial-20261123)


记录 Linux 学习过程中的知识整理、命令实验、问题排查和阶段复盘。使用 GPT 辅助解释与练习，以实际执行结果作为掌握情况的依据。

## 当前进展

- [网络编程完整总结](labs/rk3568-network-programming/docs/task-summary.md)
- [实现源码与复现入口](labs/rk3568-network-programming/README.md)
- [过程、结果与排错索引](notes/rk3568-network-programming.md)


**最新（2026-11-22）：RK3568 TCP/UDP 核心功能与真实 Wi-Fi 断网恢复已验证，源码、过程和阶段总结已归档；性能仍有保留项，一小时稳定性按用户要求暂缓。** [本轮记录](journal/2026-11-22-network-tcp.md)。以下保留阶段历史。

2026-11-22，串口出现仅回显、shell 不执行命令的故障；用户授权复位后，助手已验证恢复交互。旧 /tmp 程序已清空，Ubuntu 源码和产物仍在；此前按键队列测试通过记录保留。用户截图确认网络查询正常返回；用户最新明确要让 RK3568 接入 Windows 当前同一 Wi-Fi，无线工具检查已确认 connmanctl、wpa_supplicant、wpa_cli、udhcpc 可用；用户已在板端屏幕连接同一 Wi-Fi，截图显示 wlan0 链路就绪；板端地址和路由已确认，Windows ping 板端 3/3 成功且 SSH 端口可达；板端 ping Windows 无回应，SSH 主机指纹已核验，按用户授权添加项目公钥后免输密码登录成功；Wi-Fi scp 传输、程序校验及重新部署后的板端自测均通过，下一步实现 TCP 与心跳。

2026-11-22，用户确认网络编程方案中的 Zynq7020 统一按 RK3568 适配。继续 task2_network_programming；截图确认 adc-keys/event7 的 KEY_VOLUMEUP（115）三次短按及一次约 2.18 秒长按均有完整按下与释放，按键基础读取验证通过；已确认 System V 消息队列接口存在、当前列表为空、单条消息上限 8192 字节、队列默认容量 16384 字节。共享访问已恢复，按键与队列最小 C 程序已写入 task2；Ubuntu --self-test 已通过（sent=4 received=4 PASS），已验证本机队列收发与合成事件过滤；ARM64 交叉编译和产物检查已通过，串口包已准备；手动传输失败后，用户授权助手接管串口；现已完成传输和双重 SHA-256 校验，RK3568 自测 sent=4 received=4 PASS，队列已释放。真实按键与自编程序集成测试也已通过（sent=4 received=4 PASS）。网络方案以用户最新的同一 Wi-Fi 要求为准，准备无线连接后再进行 TCP；板端无线地址及连接状态待确认。

2026-11-21，已建立共享任务目录 RK3568_CODE/task2_network_programming 及 readme.md，说明按键、消息队列与 TCP/UDP 心跳实验；下一步核实 RK3568 按键和网络环境，实验待执行。

2026-11-21，已完成网络编程试验方案内容分析。下一步确认使用 Zynq7020 还是现有 RK3568，并核实按键接口与网络条件；TCP/UDP 实验均待执行。

2026-11-21，Linux 静态库与动态库实操已完成并收口：Ubuntu 与 RK3568 上的静态版、动态版均通过 17/17 项检查，完成依赖与体积比较、缺库与恢复测试及产物清理。源码和交叉工具链保留，基础路线 L00 尚未单独验收。

- [本次完整实操文档](notes/static-dynamic-libraries-rk3568.md)：实现流程、验证结果、问题与解释。
- [实验源码与复现入口](labs/static-dynamic-libraries/README.md)：C 代码及 Makefile。
- [本次收口日志](journal/2026-11-21-static-dynamic-libraries.md)：结果与复习要点。

- [学习路线](roadmap.md)：基础课程和 RK3568 后续方向。
- [学习进度](progress.md)：完成状态、验收证据与复习安排。
- [首次任务](lessons/L00.md)：确认 Linux 环境和终端。
- [学习日志](journal/2026-11-18.md)：记录每次学习过程。
- [记录模板](templates/session.md)：新增日志时复制使用。
- [实验目录](labs/README.md)：保存原创练习脚本及复现说明。

## 记录方式

每次选择一个小目标，先理解概念，再执行实验、解释结果、完成变式练习，最后更新日志和进度。日志放在 `journal/`，以日期命名；专题笔记放在 `notes/`；实验文件放在 `labs/`。

记录命令时注明操作系统、shell、工作目录及必要前提。区分预期结果与实际结果；尚未执行的实验标记为待执行。

## 内容范围

本仓库公开保存自主整理的学习笔记和实验代码。RK3568 厂商教材、SDK、镜像、虚拟机和下载包保留在本地资料目录。引用资料时记录来源和章节，不直接上传整套教材。公开日志中不记录密码、令牌、私钥或未脱敏的连接信息。
