# Linux 静态库与动态库实操记录

日期：2026-11-21。状态：本次实操已完成并收口。

本次用同一套四则运算代码生成静态库和动态库，先在 Ubuntu x86_64 上验证，再交叉编译为 ARM64 程序，通过 Windows 串口传入 RK3568。两端的静态版和动态版均通过 17 项检查，板端还完成了动态库缺失与恢复测试，最后清理实验产物。

源码与复现入口：[static-dynamic-libraries](../labs/static-dynamic-libraries/README.md)。

## 1 任务范围与环境

| 环节 | 实际环境 | 用途 |
| --- | --- | --- |
| 编辑与传输 | Windows、Samba、CH343 串口、PuTTY | 编辑共享目录源码，读取 Ubuntu 产物，通过串口传输及操作板端 |
| 本机编译 | VirtualBox 中的 Ubuntu 22.04.5 LTS，x86_64 | GCC 11.4.0、GNU ar 2.38、GNU Make 4.3 |
| 交叉编译 | Ubuntu 上的厂家 Buildroot 工具链 | GCC 10.3.0、GNU ar 2.36.1，目标 aarch64-buildroot-linux-gnu |
| 开发板 | ATK-DLRK3568，aarch64，64 位用户空间 | Buildroot 2018.02-rc3，Linux 4.19.232，glibc 2.35 |
| 串口 | 本次 Windows 识别为 COM3 | 1500000 波特率，8 数据位，无校验，1 停止位，无流控 |

板端 PATH 中未找到 gcc 或 cc，因此选择在 Ubuntu 交叉编译。板端与工具链 sysroot 均为 glibc 2.35；最终兼容性由板端运行测试确认，不能仅凭 CPU 名称或库版本一致下结论。

原任务中的“创建空文件”作为准备步骤处理，实际运行前补齐函数实现和测试。缺库测试用临时改名代替立即删除库，观察后恢复；最终清理阶段再删除实验文件。

## 2 源码结构与接口

```text
static-dynamic-libraries/
├── calc.h       接口声明和除法状态码
├── add.c        calc_add
├── sub.c        calc_sub
├── mul.c        calc_mul
├── div.c        calc_div
├── main.c       17 项检查
├── Makefile     本机及交叉编译
└── README.md    复现说明
```

函数采用 `calc_` 前缀，避免自定义 `div` 与标准库 `<stdlib.h>` 中的函数重名。实验使用 `int` 整数运算：加减乘要求结果在 `int` 范围内；整数除法向零截断，例如 `7 / 2 = 3`、`-7 / 2 = -3`。

`calc_div(a, b, &result)` 返回状态码，通过第三个参数返回商。它检查除数为零、`INT_MIN / -1` 溢出和空输出指针；出错时不改写输出变量。测试同时覆盖正确结果和这些错误分支，不将加减乘的整数溢出视为已处理。

## 3 Ubuntu 静态库与动态库构建

### 3.1 准备工具

初次检查时 gcc 和 ar 不可用。用户在 Ubuntu 安装基础开发工具后，分别用 `gcc --version`、`ar --version`、`make --version` 验证成功。

下文命令在 Ubuntu Linux shell 中执行，工作目录为源码所在目录。它们用于复现，不代表需要重新执行已经完成的实验。

```bash
make static
./build/native/bin/main_static
make dynamic
LD_LIBRARY_PATH="$PWD/build/native/lib" ./build/native/bin/main_dynamic
```

两种构建过程分别是：

```text
静态库：四个 .c → 普通 .o → ar rcs → libcalc.a → 与 main.c 链接 → main_static
动态库：四个 .c → -fPIC 的 .o → gcc -shared → libcalc.so → 与 main.c 链接 → main_dynamic
```

Makefile 将普通目标文件放在 `static/`，位置无关目标文件放在 `pic/`，避免混用。静态版直接指定 `libcalc.a` 的路径；若同时存在 `.a` 和 `.so`，只使用 `-lcalc` 通常会优先选到共享库。

`-L` 指定链接时的库搜索目录，`LD_LIBRARY_PATH` 指定运行时的动态库搜索目录。命令前的变量赋值只影响该次进程，不修改系统全局配置。

### 3.2 检查运行依赖

对本次自行构建的本机程序执行：

```bash
ldd ./build/native/bin/main_static
LD_LIBRARY_PATH="$PWD/build/native/lib" ldd ./build/native/bin/main_dynamic
```

| 程序 | 实际观察到的关键动态依赖 |
| --- | --- |
| main_static | libc.so.6；没有 libcalc.so |
| main_dynamic | libcalc.so 指向实验目录，并依赖 libc.so.6 |

`main_static` 的含义是计算库采用静态链接。它仍依赖系统 C 库，因此 `file` 显示 `dynamically linked` 并不矛盾。

## 4 文件体积与两端结果

Ubuntu 使用 `stat -c '%n : %s bytes'` 读取完整文件的字节数；ARM64 数值来自对共享目录产物的 Windows 侧文件大小检查。

| 文件 | Ubuntu x86_64 字节数 | RK3568 ARM64 字节数 |
| --- | ---: | ---: |
| main_static | 16368 | 9672 |
| main_dynamic | 16248 | 9496 |
| libcalc.a | 5260 | 5476 |
| libcalc.so | 15320 | 8328 |

在 Ubuntu 上，动态版可执行文件自身小 120 字节；但单个动态程序加计算共享库共 31568 字节，而静态版程序为 16368 字节。运行静态版不需要携带 `.a`。这里不计两者共同依赖的系统库，也不是运行内存占用比较。

这个计算库很小，共享库文件本身也有 ELF、符号和对齐等开销；不能由此断言动态链接总是更大。多个程序共享同一个库时，应按共享一次的库体积计算。跨架构大小还受到编译器和默认链接配置影响，不能单独归因于 CPU 架构。

| 环境 | 静态版 | 动态版 | 验证依据 |
| --- | --- | --- | --- |
| Ubuntu x86_64 | 17/17 通过 | 17/17 通过 | 用户终端截图 |
| RK3568 ARM64 | 17/17 通过 | 17/17 通过 | 用户 PuTTY 截图 |

四个组合的汇总均为 `Result: 17/17 checks passed.`。结果一致的范围是本次 17 项检查，不覆盖任意输入或其他系统。

## 5 交叉编译到 RK3568

Ubuntu 的本机 GCC 生成 x86_64 程序，不能直接作为 ARM64 程序在板端运行。本次使用 Linux 4.19 资料配套的独立工具包：

```text
atk-dlrk3568-toolchain-aarch64-buildroot-linux-gnu-x86_64-20230921-v1.0.1.run
```

安装包复制到实验目录后，源与副本的 SHA-256 校验一致。安装器通过 `-d` 安装到实验目录的 `build/toolchains/atk-rk3568-4.19`，不改全局 PATH；实际编译器在其 `bin/` 下。已验证 `--version`、`-dumpmachine` 和 `-print-sysroot`，目标为 `aarch64-buildroot-linux-gnu`。

执行环境仍是 Ubuntu。若工具链位置不同，先将 `TOOLCHAIN` 改为实际安装目录：

```bash
TOOLCHAIN="$PWD/build/toolchains/atk-rk3568-4.19"
make BUILD_DIR=build/rk3568 CC="$TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-gcc" AR="$TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-ar" all
file build/rk3568/bin/main_static build/rk3568/bin/main_dynamic build/rk3568/lib/libcalc.so
"$TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-readelf" -l build/rk3568/bin/main_dynamic
"$TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-readelf" -d build/rk3568/bin/main_static
"$TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-readelf" -d build/rk3568/bin/main_dynamic
```

实际产物均为 ELF 64-bit ARM aarch64；程序解释器为 `/lib/ld-linux-aarch64.so.1`，板端存在同一路径。`NEEDED` 检查显示静态版只有 `libc.so.6`，动态版还有 `libcalc.so`。

检查交叉编译产物使用 `file`、交叉 `readelf`，不在 x86_64 虚拟机直接执行 ARM64 程序，也不使用宿主机 `ldd` 来判断板端加载情况。更换工具链时使用新的构建目录，避免复用旧 `.o`。

## 6 串口传输与板端运行

### 6.1 编译和传输可以由不同系统承担

```text
Ubuntu 交叉编译
    ↓ Samba 共享目录
Windows 读取 ARM64 产物
    ↓ COM3 串口
RK3568 接收、校验并运行
```

板端两个以太网接口检查时均无载波，没有非回环 IPv4 地址，所以使用已经连通的串口。Windows 只搬运文件，没有重新编译或改变程序架构。

本次传输 `main_static`、`main_dynamic`、`libcalc.a`、`libcalc.so` 和 `calc.h`，共 33404 字节。文件打包后为 7240 字节，使用 Base64 分块经 Windows 的串口接口写入板端。压缩包先校验，再解包，五个文件的 SHA-256 全部通过。`.a` 和头文件是按实操要求一起传送的，并非运行静态版所必需。

板端文件位于独立的 `/tmp/rk3568-calc-20261121-f30e6c52`，包含 `bin/`、`lib/`、`include/`。实测 `/tmp` 为 tmpfs，文件通常随重启消失。本次没有刷机、替换系统库或修改网络与启动配置。

板端 BusyBox tar 不支持 `-z`，因此使用 `gzip -dc` 先解压，再用 `tar -xkf` 解包。`-k` 用于避免覆盖已有文件。

### 6.2 从 PuTTY 进入板端

Windows PuTTY 选择 Serial，连接本次识别到的 COM3，设置 1500000、8N1、无流控。按回车后出现开发板的 root 提示符。COM 口编号需按自己的设备识别结果确认，同一串口通常不能被两个程序同时占用。

以下是开发板 Linux shell 命令，在 PuTTY 中执行：

```bash
cd /tmp/rk3568-calc-20261121-f30e6c52
./bin/main_static
LD_LIBRARY_PATH="$PWD/lib" ./bin/main_dynamic
```

两者均通过 17 项检查。文件传输不会自动启动程序；程序只向当前终端打印结果，不创建图形窗口，也不控制 LED。

## 7 动态库缺失与恢复

在板端实验目录临时将 `lib/libcalc.so` 改名为 `lib/libcalc.so.disabled`，没有改动系统 `/lib`。

| 操作 | 预期现象 | 本次结果 |
| --- | --- | --- |
| 改名后运行静态版 | 仍通过 17 项检查 | 用户文字确认符合预期 |
| 改名后运行动态版 | 启动时报找不到 libcalc.so | 用户文字确认符合预期 |
| 恢复名称后运行动态版 | 重新通过 17 项检查 | 用户文字确认符合预期 |

本轮通过改名模拟库缺失，没有保留新的原始报错截图；最终清理时再删除库文件。它与此前“未设置 LD_LIBRARY_PATH 导致库找不到”的排错属于不同实验步骤。

关键原理是：静态链接时，链接器已将所需计算代码加入 `main_static`，运行时不再读取 `libcalc.a`；动态版则仍需要加载 `libcalc.so`。`.a` 并不是需要一直放在可执行文件旁边的运行资源。

## 8 排错记录

| 现象 | 原因或判断 | 处理与结果 |
| --- | --- | --- |
| gcc、ar 命令找不到 | Ubuntu 开发工具未准备好 | 安装后核对三项工具版本，后续编译通过 |
| VS Code 的系统头文件标红 | 编辑器环境与 Ubuntu 编译环境可能不同 | 不复制系统头文件；Ubuntu 实际编译确认可以找到头文件 |
| main_dynamic 找不到 libcalc.so | 链接目录不会自动成为运行时搜索路径 | 为单次运行设置 LD_LIBRARY_PATH，17 项检查通过 |
| cd /RK3568_CODE/... 失败 | 前导 / 指向 Linux 根目录 | 回到实际实验目录，再使用依赖当前目录的路径 |
| 输入 ake 后命令找不到 | make 的首字母 m 遗漏 | 完整复制命令，不安装无关软件，编译成功 |
| main_static 显示 dynamically linked | 只对计算库静态链接，仍依赖系统 libc | 用 ldd/readelf 确认依赖，不把文件名当作链接方式的证明 |
| Windows 无法再次打开 COM3 | PuTTY 正占用串口 | 保留用户终端，由用户运行和清理，避免抢占串口 |
| tar -xzf 报不支持 z | 板端 BusyBox 裁剪了该选项 | 分开执行 gzip 解压与 tar 解包，校验通过 |
| 传输后开发板屏幕没有变化 | 传输不等于运行，测试程序也没有 GUI | 在 PuTTY 中启动程序并观察标准输出 |

## 9 清理与任务收口

Ubuntu 共享目录中的 `build/native` 和 `build/rk3568` 已清理，包含各自的目标文件、库和可执行程序。助手先核对文件范围，随后通过 Windows Samba 逐文件删除并移除空目录，检查产物目录不存在。源码、头文件、Makefile、交叉工具链及其安装包保留。

板端由用户在 PuTTY 中删除独立实验目录里的程序、库、头文件及传输中间文件，再移除空目录。2026-11-21 用户明确反馈“已完成清理”，据此将清理记为完成；没有新增板端清理截图。

本地传输压缩包已删除，校验记录保留。GitHub 仅归档源码、构建规则与学习文档，不上传工具链、厂商教材、系统镜像或编译产物。

| 验收项 | 状态 |
| --- | --- |
| Ubuntu 静态库与动态库编译运行 | 通过 |
| 运行依赖与文件体积比较 | 完成 |
| 交叉编译及 ARM64 产物检查 | 通过 |
| 串口传输与文件校验 | 通过 |
| RK3568 两种版本运行 | 通过 |
| 库缺失与恢复 | 用户确认通过，采用临时改名 |
| 两端实验产物清理 | 完成，板端依据用户确认 |

这次实操任务收口，不等同于基础路线 L00–L09 全部验收。仍需复习静态链接与动态链接的阶段差异，以及相对路径、绝对路径和当前目录的关系。

## 10 资料来源

- 本地《Linux 静态库与动态库实操任务文档》：任务范围依据。
- 正点原子《基于 Buildroot 系统 交叉编译器安装与使用参考手册 V1.0》：安装与使用章节；工具包选择 Linux 4.19 目录中的 RK3568 版本。
- 实际终端截图、Windows 文件检查、串口校验记录及用户对缺库实验和清理的明确反馈：本文结果依据。文档仅整理实验与引用资料名称，不转载厂商资料。
