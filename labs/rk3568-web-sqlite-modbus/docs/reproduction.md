# 构建、运行与排错说明

## 环境与命令入口

Ubuntu复现命令集中在本目录commands.txt。需要已准备的匹配SDK、主机和AArch64 libmodbus3.2.0前缀、SQLite头文件和库；脚本不安装软件、不修改网络。公开构建入口需要显式设置TASK5_TOOLCHAIN、TASK5_HOST_MODBUS、TASK5_TARGET_MODBUS；管理工具验证另传TASK5_TCP_BUILD。

build_tcp.sh生成新build/tcp.*，运行30协议+10诊断+17C主站测试并交叉构建。build_phase4.sh可复现Web/SQLite/配置124项回归和对应程序。check_tcp_ops.sh校验已构建TCP清单，再验证14管理+3模拟工具测试，复用C产物。先读脚本提示和依赖前提，不应在Windows或板端误执行Ubuntu构建命令。

原本地入口已实际通过；公开入口只把外部路径参数化并为新目录创建build、移除本地SSH编排引用，尚未在此公开路径重新构建。C源码与原验证快照一致，source-integrity.json提供核对。

## 目标运行边界

产物位于board/bin和board/lib，编译器为AArch64，主机程序位于host；不得混用。公开源码不直接部署任何程序，目标启动需已核实地址、端口、接口和运行权限。502是低端口，沿用环境授权运行，不能擅自改验收端口或全局权限。

服务端参数interface/bind/port/unit/duration选择实际接口、监听和限时窗口；默认仅回环1502，与板端正式验收502区分。新私有部署目录700，私有libmodbus配合ORIGIN相对路径。当前实验使用/tmp，重启后可能消失；本次已停止任务服务，源码发布不再次启动。

TCP管理工具start/serve/status/stop只处理自己的子进程，通过私有Unix控制接口和flock避免重复启动。root必须绝对规范路径、当前用户拥有、0700；完整control.sock路径≤107字节。建议短工作路径，过长Unix路径会拒绝。管理器参数duration默认300、最多7200秒；本轮30分钟服务限时1860秒。

单次日志最多1MiB但仍排空管道；每10秒记录CPU/RSS/FD。旧run目录不自动轮换，总容量仍需管理。SIGKILL/管理器崩溃不能保证子进程立即回收；子进程有自身期限，状态不明时不根据旧PID盲目kill。不是常驻系统服务或自动故障恢复方案。

## 只读主站和上位机

C主站参数host（数字IPv4）、port、unit、count、interval-ms；默认502/Unit1/5次/1000ms。本次验收Unit7，使用者必须与本次服务器一致。PC Modbus工具选择TCP、FC03、起始0、64字，地址显示模式参照点表。错误Unit可能等待超时，不等于服务器未实现。

tcp_soak.py跨Windows/Linux，默认1800秒、3客户端1Hz，保存固定门槛和全部请求日志；失败后重连但本轮总结果仍FAIL。完成时需检查summary和服务正常停止，而非只看终端出现连接成功。

## Web与数据库复现

make_web_config.py根据实际目录和bind生成独立nginx配置，默认只读；admin选项额外启用回环管理站。prepare_config_runtime.py在新私有目录生成临时令牌和配置库，不输出令牌。上述准备不是授权启动系统服务或更改网络，管理入口应通过已授权SSH转发使用。

历史库由状态服务显式--db启用；未传则不写历史。配置库与runtime-file成对启用，不能同时以unit/history-interval覆盖保存值。TCP-only管理工具不启用这些选项；如要复现Web/DB联合须按架构分别设置，不将TCP-only工具误称完整启动器。

## 常见现象

502无监听：可能是限时任务已正常停止；先查看任务状态和日志，不重置网络。3秒空闲后断开：符合当前服务策略，主站保持轮询或重连。历史50条：是分页上限。曲线平：信号可能稳定。RTU未启用：符合本阶段范围，与PuTTY无关。IP/时间/信号未知：检查有效位，不强填正常数值。
