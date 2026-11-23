# 任务03 选做功能与操作说明

## 当前状态

2026-11-23：接收文件日志、原始接收快照、分块发送文件及一键构建/推送入口已写入源码和scripts目录，保留应用内横屏、触摸键盘与可滚动选择面板。选做功能版已交叉编译、部署并通过24项板端自检；真实4096 B文件发送、双向二进制、12 B文本与96 KiB接收日志/64 KiB快照均已核验。文件取消、64 MiB上限的实机故障边界仍待验证。

## 接收记录与保存

- “保存当前”：将当前视图保留的原始字节原子保存为snapshot-*.bin；最多64 KiB，不包括先前裁剪的数据，不受ASCII/HEX显示模式影响。空视图给出提示。
- “记录接收”：串口打开后点击，开始记录此后收到的全部原始字节；不添加时间戳、不按readyRead分块插入分隔符。再次点击停止。每文件最多64 MiB，达到上限或写文件失败时停止记录并提示，串口接收继续。
- 接收记录生成rx-*.bin及同名.bin.json，包含端口参数、起止状态、字节数和SHA256。异常中止保留已写文件与状态，不冒充完整记录。
- 文件位于可执行程序旁data/received。当前部署在/tmp，重启可能丢失，需在重启前通过固定通信通道取回任务目录。关闭串口或退出应用会结束记录。
- 清空接收窗口不会删除文件，也不停止正在进行的接收记录。记录按钮不能证明已收到数据，须核对文件字节数及对端序列。

## 发送文件

- 串口打开且当前发送队列排空后，点击“发送文件”，在应用内文件列表选择普通文件，再点击“发送所选文件”。默认目录为程序旁data/send，可进入其他可读目录。
- 支持1 B至64 MiB文件；按原始字节发送，不做文本编码，不追加LF/CRLF，空文件、目录、设备和管道不作为有效发送源。
- 每次读取最多4096 B，前一块排空后才读取下一块；与文本和定时发送互斥，开始文件发送会停止定时发送。不会因文件大小增加而无限增长队列。
- “取消文件”会关闭串口并取消待写数据。已经写出的字节无法撤回；需要重新打开后才能再次发送，自动发送不恢复。
- 进度为本机bytesWritten计数。完成时输出读取数据的SHA256，须与对端保存文件比较，才可认定发送准确。文件读取失败/大小变化或串口异常将中止传输。

## 构建与推送入口

### Ubuntu构建

scripts/build.sh使用已验证的厂商Qt5.15.2 SDK与aarch64-buildroot-linux-gnu-g++，默认写入build/qt_demo-rk3568-cli，与Qt Creator构建目录分开；输出qt_demo、SHA256SUMS、build-info.txt和qt_serial_assistant.tar.gz。无sudo、无安装，不修改全局环境。Ubuntu待执行命令仅在本任务commands.txt。

可选环境变量RK3568_SDK、RK3568_BUILD_DIR、JOBS；构建目录必须留在本任务build内。脚本已通过Windows Git Bash的bash -n静态语法检查，尚未在Ubuntu实际构建。

### Windows推送

推荐scripts/deploy.py（Python3标准库）。默认上传Qt Creator的Release产物；--build cli选择build.sh产物；--plan只检查本地产物并打印计划，不联网。执行环境为Windows终端，工作目录为任务目录。

```powershell
python .\scripts\deploy.py --ssh-config C:\path\to\ssh_config --plan
python .\scripts\deploy.py --ssh-config C:\path\to\ssh_config
```

使用CLI构建产物时：

```powershell
python .\scripts\deploy.py --ssh-config C:\path\to\ssh_config --build cli
```

脚本复用本机固定SSH配置，不复制私钥或改认证；新建独立板端/tmp目录，检查AArch64、SHA256、ldd和--self-test，上传已知4096 B测试文件到data/send，写入evidence/script-*/证据。不会启动GUI或打开串口。缺失依赖、校验失败或自检失败立即停止。

公开版仅保留Python推送入口，不要求降低PowerShell执行策略。Python入口的语法、--plan检查及真实推送流程均已通过；证据在evidence/script-20261123-152436-a2cf8278。

## 新增自检与样例

新版--self-test保留10项SerialData自检，新增14项FileIO测试：原子快照、二进制分块、SHA256、原始日志拼接、容量上限、元数据、目录拒绝、写失败、短写与零写处理。只使用临时测试文件，不启动图形窗口，不打开串口。目标实际执行结果为SerialData 10/10、FileIO 14/14全部通过。

测试样例tests/fixtures/all-bytes-4096.bin为00至FF循环16次，SHA256为c8f5d0341d54d951a71b136e6e2afcb14d11ed8489a7ae126a8fee0df6ecf193。它可用于“发送文件”的对端逐字节验收，也可由对端发送给板端用于接收记录验证。

## OTG虚拟串口实际配置与恢复

2026-11-23用户明确授权临时启用ACM并保留ADB。板端函数acm.gs6/port_num=0对应ttyGS0，配置中同时有ffs.adb与acm.gs6，UDC configured/high-speed；Windows识别USB串行设备COM4（VID2207/PID0019/MI01）。COM3仍为CH343控制台，未用于测试。

仅修改/tmp/.usb_config，保留usb_adb_en并加入usb_acm_en；/etc/init.d/.usb_config持久配置哈希未变。首次单独stop触发udev自动拉起，查明61-usbdevice.rules和/usr/bin/usbdevice的行为后，采用厂商原生restart处理USB_FORCE_CHANGED标志成功，不禁用监控。不要继续照早期“先stop等待静止”的候选顺序操作。

实际备份位于/tmp/task3-otg-acm-backup.qQcBGh/usb_config.original。回退时先退出虚拟串口会话，核对备份，将原临时配置原子恢复后使用厂商原生restart，并核对仅ADB绑定、UDC和持久配置哈希。回退会再次短暂中断USB，本轮没有演练回退；精确过程和待执行命令保存在docs/otg-setup.md。临时目录重启后可能消失，届时重新读取实际配置，不能假设旧备份仍在。

--allow-usb-gadget参数仅在用途核实后允许手动选择ttyGS0，仍保护活动控制台/ttyFIQ0；它不配置USB、不自动打开串口。默认继续保留ttyGS0。OTG CDC ACM已用于本轮真实验证，但不能证明ttyS3 RS-232电平、物理接线或UART波特率时序。

## 已验证与待验证的选做边界

|项目|实际通过|待补充|
|---|---|---|
|接收日志/快照|12 B文本、4096 B二进制、98304 B完整日志与65536 B尾部快照；逐字节、元数据、SHA256一致|64 MiB上限、存储耗尽、断电持久性不作通过声明|
|文件发送|4096 B全字节样本到电脑精确一致，未追加CRLF|取消文件、文件中途变化、大文件长期负载|
|触摸适配|实际点击、输入、选择、横屏、停止/保存操作|完整鼠标与所有弹出面板组合回归|
|脚本|Windows一键推送和板端自检通过|Ubuntu一键构建/打包实际执行|
