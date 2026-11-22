# RK3568 Linux 网络编程实验

按原《Zynq7020开发板Linux网络编程试验方案》适配 RK3568，实现按键采集→消息队列→TCP/UDP→Windows 接收、日志和心跳监测。

**阶段结果（2026-11-22）：核心功能与 TCP/UDP 真实 Wi-Fi 断网恢复已验证；完整性能未全部通过。一小时稳定性按用户要求暂缓，未执行。**

- [完整任务总结](docs/task-summary.md)
- [TCP 实施过程](docs/tcp-stage.md)、[UDP 实施过程](docs/udp-stage.md)
- [日志修正与验证](docs/tcp-log-fix-validation.md)、[根因分析](docs/tcp-root-cause-analysis.md)
- [UDP 真实断网恢复](docs/udp-outage-validation.md)
- [结果证据说明](logs/README.md)

## 实现和结果

板端主要使用 C：约 2ms 采样、10ms 稳定消抖、System V 消息队列、独立网络线程、1Hz 心跳。协议为 6 字节心跳与 8 字节按键帧，使用大端时间戳和 XOR。
Windows C# 接收器支持 TCP 分帧和 UDP 数据报校验；超过 2 秒没有有效心跳报警；有界异步日志按实际写完计时。

| 验证 | 已执行结果 |
| --- | --- |
| UDP 合成事件 | 入队/发送/接收均 48；心跳 11/11 |
| UDP 真实按键 | 按下、释放、入队、发送、接收各 4；长按约 1.98 秒只报一次 |
| UDP 三分钟运行 | 心跳 180/180；184 条报文日志最大 33.182ms |
| UDP 真实约 8 秒断网 | 约 2001ms 报警，自动恢复心跳与事件；成功发送按键 229/229、心跳 47/47 收到 |
| 断网丢失 | 44 个合成按键本地发送失败且未补发；INCOMPLETE 如实保留，不等于程序崩溃 |
| 保留项 | 心跳到达间隔 25/179 超差；精确物理/单向延迟未验收；一小时稳定性暂缓 |

## Ubuntu：本机检查与交叉编译

需要已有 GCC、Make、Python 3 与适用于板端 ABI 的交叉工具链；下列命令不安装软件或修改网络。

```sh
cd labs/rk3568-network-programming
make check CFLAGS='-O2 -g -Werror'
# 改为自己已安装的工具链路径：
toolchain_dir=/path/to/aarch64-toolchain
make BUILD_DIR=build/rk3568 CC="$toolchain_dir/bin/aarch64-buildroot-linux-gnu-gcc" CFLAGS='-O2 -g -Werror'
```

生成 key_network（TCP）、key_udp（UDP）、key_queue 和 test_core。TCP/UDP 共用 key_network.c 的按键和队列主流程，UDP 构建包含 udp_transport.inc。

## Windows PowerShell：构建与回归

需要 Windows 已有的 .NET Framework C# 编译器；源码从 Git 克隆至本地后运行：

```powershell
cd labs/rk3568-network-programming
& .\host\build.ps1
python .\tests\test_receiver.py .\build\windows\Receiver.exe .\build\windows\tcp-regression.log
python .\tests\test_udp_receiver.py .\build\windows\Receiver.exe .\build\windows\udp-regression.log
```

UDP 回归使用新的日志文件名，重复运行时请更换名称。不要为了运行脚本降低系统执行策略。

## 板端与上位机运行

先确认开发板使用的 input 设备支持 key code 115，不能照搬设备编号或按电源键。上位机端口与防火墙按实际实验环境配置；本仓库脚本不会自动开放端口。

Windows PowerShell（把示例地址换成真实地址；创建本地日志，60 秒后退出）：

```powershell
$hostIp = '192.0.2.20'
$boardIp = '192.0.2.10'
$log = Join-Path $PWD ('build\\windows\\udp-' + (Get-Date -Format yyyyMMdd-HHmmss) + '.log')
& .\build\windows\Receiver.exe $hostIp 8888 60 --udp --peer $boardIp --log $log
```

开发板 Linux shell（将交叉产物部署到有执行权限的实验目录后）：

```sh
./key_udp 192.0.2.20 8888 /dev/input/event7 50
```

TCP 使用 key_network；Windows 去掉 --udp 和 --peer 参数。参数地址来自文档示例网段，不能直接用于实际联网。

## 发布范围和证据边界

公开材料为原创源码、通用测试、脱敏过程文档与结果 JSON；不包含厂商原文/SDK、工具链、二进制、抓包、私钥或实际连接配置。
源码采用 Ubuntu 编译时校验应用的 UDP 版本快照，避免 Windows 共享缓存旧视图；传输逻辑与已验证版本一致。host/build.ps1 仅将默认输出目录改为仓库内 build/windows，便于复现。
本次发布没有重新运行板端实验，不代表另一次性能验收。历史失败、无线抖动和未测项目均保留；不将其归因为 RK3568 性能不足。
