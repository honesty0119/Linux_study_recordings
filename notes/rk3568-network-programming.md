# RK3568 网络编程：实现、过程与总结入口

2026-11-22，依据网络编程实验方案完成 RK3568 适配。核心开发和 TCP/UDP 真实 Wi-Fi 断网恢复已有验证；一小时稳定性按用户要求暂缓，性能保留项仍未全部通过。

## 阅读顺序

1. [实验入口与源码复现](../labs/rk3568-network-programming/README.md)
2. [完整阶段总结与验收表](../labs/rk3568-network-programming/docs/task-summary.md)
3. [TCP 实施过程](../labs/rk3568-network-programming/docs/tcp-stage.md)与[UDP 实施过程](../labs/rk3568-network-programming/docs/udp-stage.md)
4. [日志修正](../labs/rk3568-network-programming/docs/tcp-log-fix-validation.md)、[时序归因](../labs/rk3568-network-programming/docs/tcp-root-cause-analysis.md)
5. [UDP 真实断网恢复](../labs/rk3568-network-programming/docs/udp-outage-validation.md)

## 本次学到的关键点

Ubuntu 负责交叉编译，RK3568 执行 AArch64 程序，Windows 负责接收和分析。共享文件不等于远程编译；编译通过也不等于板端验收通过。

按键采集与网络通过消息队列解耦。心跳采用独立的单调时钟周期，不能被无事件等待或连续按键阻塞。TCP 按字节流处理半帧/合帧，UDP 保留数据报边界且不保证送达。

send/sendto 成功不等于上位机收到；UDP 恢复通信也不等于补发离线事件。日志改为异步后，计时仍必须包含排队和实际输出，不能拿入队时间替代打印完成时间。不同机器未同步的时钟不能直接相减求单向延迟。
