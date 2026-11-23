# 任务03 环境与工程交付指南

## 已确认环境

|层次|当前配置|
|---|---|
|Ubuntu主机|22.04.5 LTS，x86_64|
|IDE|Qt Creator 6.0.2，RK3568 Qt5.15.2 Kit|
|交叉编译器|GCC/G++10.3.0，aarch64-buildroot-linux-gnu|
|目标Qt|厂商SDK Qt5.15.2，Core/Gui/Widgets/SerialPort|
|开发板|ATK-DLRK3568，aarch64，Linux4.19.232，Buildroot2018.02-rc3|
|显示|现有Weston/Wayland，XDG_RUNTIME_DIR=/var/run，WAYLAND_DISPLAY=wayland-0，QT_QPA_PLATFORM=wayland|
|LCD|DSI原生1080×1920；已读到的应用逻辑表面720×1280|
|字体|目标Qt实际选择Noto Sans SC，中文显示已有照片|

## SDK与Kit

SDK根目录：/opt/rk3568-sdk。

qmake位于该目录bin/qmake，是在Ubuntu x86_64执行的主机工具。交叉gcc/g++同在bin下，名称aarch64-buildroot-linux-gnu-gcc/g++。目标sysroot为该目录aarch64-buildroot-linux-gnu/sysroot，Qt目标库位于sysroot/usr/lib，头文件位于sysroot/usr/include/qt5，平台插件位于sysroot/usr/lib/qt/plugins/platforms。

Qt Creator登记SDK qmake、交叉C/C++编译器及此sysroot，关联RK3568 Qt5.15.2 Kit。Device type为Generic Linux Device，Build device为Local PC。未配置远程设备与调试器只影响IDE远程运行/调试，已有qmake构建成功证据。不要用Ubuntu本机Qt库链接目标程序，也不要在Ubuntu直接运行ARM产物。

配置细节见本文SDK与Kit章节；其中按时间记录的早期“待配置”状态是历史。当前Kit/交叉构建/部署链路已实证通过。Ubuntu安装和后续待执行命令只保存在本任务commands.txt。

## 工程创建与结构

从Qt Widgets Application创建qmake工程qt_demo，使用C++11。任务特性需要Qt C++/信号槽，符合项目允许特定任务使用其他语言的约定。

- main.cpp：字体、参数、自检及外层全屏窗口。
- mainwindow.ui、mainwindow.cpp/.h：界面、串口状态与事件驱动收发。
- landscapeview.cpp/.h：仅应用内旋转、选项面板、指针坐标映射，不改Weston。
- serialdata.cpp/.h：原始字节、显示与有界队列数据规则。
- serialextras.cpp/.h：日志与文件发送控制、页面内文件选择。
- fileio.cpp/.h：原始日志/原子快照/文件分块和文件自检。
- serial_ui.qrc及icons：内嵌资源，随可执行文件部署。

Qt模块依赖core、gui、widgets、serialport。构建qmake spec为devices/linux-buildroot-g++。源码/资源变化后先Run qmake再Build。Creator Release输出位于build/qt_demo-rk3568-release；CLI入口与产物见optional-features.md。

## 部署与运行

固定通信入口是Windows OpenSSH/scp，别名rk3568-board，配置用户自行配置的SSH配置文件。UbuntuSSH密钥并未因此自动配置。配置与私钥不进入共享任务或公开仓库。

新产物推送到独立/tmp目录，保留旧版以便回退，核对两端SHA256、ELF架构和ldd，再执行--self-test。当前推荐一键入口scripts/deploy.py，详见optional-features.md。启动命令按本次实际部署路径在聊天中提供，沿用上述Wayland环境，不停止厂商桌面。

默认仅串口助手旋转90度；--rotate-left反向，--windowed用于窗口验证；--allow-usb-gadget仅在核实USB虚拟串口用途后允许手动选ttyGS0。退出按钮关闭串口、停止定时/文件发送和接收记录。不能把退出GUI理解为改变系统屏幕方向。

QStandardPaths对/var/run符号链接/权限的提示已有记录，应用可运行；不为消除此提示擅自chmod /run。ttyS3曾输出DMA申请失败后使用interrupt mode，随后打开成功；真实吞吐和准确性仍需测试。

## 验收与证据入口

当前范围见requirements-status.md，逐项矩阵见requirements-status.md。构建/部署/哈希/自检证据在evidence，LCD与终端截图在对应版本目录。当前选做功能版24项板端自检通过，OTG双向文本/4096 B二进制、96 KiB接收日志/64 KiB快照及热拔插恢复均有实证。长时负载、实体UART参数、权限不足等保留项见requirements-status.md。原始任务书original-task.md保留。

## 已验证版本与复现顺序

当前已验证产物为build/qt_demo-rk3568-release/qt_demo，部署日期2026-11-23，SHA256 b48ea44b8804ada462a880e693cbdb770b90663d6997827b50673a6a858790f7。板端路径/tmp/task3-serial-package.XODNmb/qt_demo是本次实例路径，重启后不能保证存在；重新推送后以脚本返回的新路径为准。

1. 在Ubuntu Qt Creator打开qt_demo/qt_demo.pro，不直接复用他人的.pro.user。按前述SDK位置登记qmake和交叉C/C++编译器，设置sysroot，选择RK3568 Kit的Release配置。
2. 设置任务内影子构建目录，执行Run qmake，再Build；编译日志应调用SDK工具和目标Qt库。该手工构建路径已有实证。
3. 可选执行commands.txt末尾的Ubuntu build.sh命令；这一条尚待Ubuntu实际运行，不能把脚本语法检查等同构建通过。
4. 在Windows使用scripts/deploy.py，默认读取Creator Release目录；--build cli改读一键构建目录。脚本会校验架构、传输哈希、依赖和24项自检，不自动启动GUI。该Creator产物推送路径已实际通过。
5. 以脚本返回目录启动程序，使用本指南Wayland环境。当前使用--allow-usb-gadget --rotate-left；参数含义见上文，不能在未核实USB用途时默认解除保护。板端启动命令按用户约定在聊天给出。
6. 在LCD选择已确认测试口，打开后完成对端收发；先检查对端实际字节，再核对日志SHA256。完整测试程序和结果见docs/acceptance-report.md。
7. 结束时先停止自动/文件发送与接收记录，按应用退出；在重启前回收程序旁data/received中的文件。此次材料整理没有代替用户退出应用或回退USB配置。

## 运行依赖与排错边界

Qt运行库和平台插件使用板端已安装的匹配版本，资源图标由qrc嵌入；中文字体来自板端Noto Sans SC，不在包内再分发系统字体。若移到其他镜像，重新核对Qt库、libstdc++/libc ABI、平台插件及其图形依赖，不能只复制单个Qt库就认定兼容。目标程序为AArch64，qmake本身为x86_64，这是正常交叉构建分工。

Qt5.15.2是本机已验证厂商匹配基线，不表示最新版本或承诺当前安全维护状态。未评估升级Qt6所需BSP/工具链/图形栈改动。linuxfb/eglfs等插件候选并不代表本轮用过；实际走现有Wayland会话。

发生依赖缺失或平台插件错误时，保留ldd和启动日志，核对平台插件路径与Wayland会话；不因排错直接停止厂商桌面、修改/run权限或换系统图形后端。串口错误需保留端口、参数及errorOccurred信息；串口分块读取是字节流，不当作完整协议帧。

## 交付范围

GitHub Release中的ZIP包含任务源码、构建/部署脚本、已验证AArch64程序、图文文档、样例和精选测试证据；不含交叉SDK、sysroot、私钥、个人.pro.user或Qt系统运行库。SDK在任务01目录的只读复用是外部构建依赖，迁移时需重新配置实际路径。

图文报告同时提供Markdown和HTML。图片路径为相对路径，整包解压后打开docs/acceptance-report.html即可离线浏览；不要仅搬走HTML而遗漏images目录。文件哈希清单用于检查包内容完整性，不替代重新编译和板端验收。


## 公开版适配

本机路径已泛化，默认SDK示例为/opt/rk3568-sdk；设置RK3568_SDK即可覆盖。deploy.py要求显式--ssh-config并使用rk3568-board别名，未在公开目录重新部署。原本机执行证据不记为发布后的新运行。Release的runtime/qt_demo是已验证二进制，源码目录不提交可执行文件。离线HTML需连同images目录解压；GitHub直接阅读Markdown报告。公开版不包含其他任务流水、私钥或原始连接配置。
