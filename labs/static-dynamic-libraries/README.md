# 四则运算静态库与动态库实验

同一组 C 源码构建 `libcalc.a`、`libcalc.so` 和两个测试程序。2026-11-21，Ubuntu x86_64 与 RK3568 ARM64 的静态版、动态版均通过 17 项检查。实验产物已清理，可用本目录源码重新构建。

完整过程、实际结果和排错记录见 [实操文档](../../notes/static-dynamic-libraries-rk3568.md)。

## 接口与限制

`calc.h` 声明 `calc_add`、`calc_sub`、`calc_mul`、`calc_div`。前三者要求结果可由 int 表示。`calc_div` 向零截断，通过指针返回商，状态码分别为成功 0、除零 1、溢出 2、无效输出指针 3；失败不修改输出值。

`main.c` 包含 17 项检查，全部通过时输出 `Result: 17/17 checks passed.` 并返回 0，有失败则返回 1。

## Ubuntu 本机复现

前提：Ubuntu 已具备 GCC、GNU ar、Make。从仓库根目录进入源码目录，在 Linux shell 中执行：

```bash
cd labs/static-dynamic-libraries
make
./build/native/bin/main_static
LD_LIBRARY_PATH="$PWD/build/native/lib" ./build/native/bin/main_dynamic
```

也可使用 `make static`、`make dynamic` 分别构建，或用 `make run` 构建并运行本机版本。源码所在目录新增 `build/native`，不需要 sudo。

## RK3568 交叉编译

需要已安装且适配板端的工具链。本次为 GCC 10.3.0、aarch64-buildroot-linux-gnu、glibc 2.35；工具链不包含在仓库中。

仍在上述源码目录，先将下面的 `/实际安装目录` 替换为工具链根目录，不要原样执行占位符：

```bash
TOOLCHAIN=/实际安装目录
make BUILD_DIR=build/rk3568 CC="$TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-gcc" AR="$TOOLCHAIN/bin/aarch64-buildroot-linux-gnu-ar" all
file build/rk3568/bin/main_static build/rk3568/bin/main_dynamic build/rk3568/lib/libcalc.so
```

更换架构或工具链时使用不同的 `BUILD_DIR`。不要在 x86_64 Ubuntu 上执行 ARM64 产物或对其使用 `make run`。静态版本明确链接 `.a`，但仍可能动态依赖系统 libc。

## 开发板运行

传输后应保持目录结构：`bin/main_static`、`bin/main_dynamic`、`lib/libcalc.so`。本次还按任务要求传输了 `lib/libcalc.a` 和 `include/calc.h`，但两者不是已链接程序运行的必需文件。

在开发板终端进入实际接收目录后执行：

```bash
./bin/main_static
LD_LIBRARY_PATH="$PWD/lib" ./bin/main_dynamic
```

本次用 Windows COM3、1500000 波特率、8N1、无流控传输并连接 PuTTY。文件放在独立 tmpfs 目录，重启后通常消失；复现时需先确认目录存在并已完成传输。

## 清理范围

完成并保存测试结果后，确认所在机器和目录，再删除本实验的 `build/native`、`build/rk3568` 或板端独立接收目录内的实验文件。保留源文件、Makefile 和工具链。不要删除整个 `build/`，本次工具链也安装在其下。

Makefile 不提供自动删除目标。运行缺库实验时只临时改名本实验的 `.so`，观察后恢复，不操作系统 `/lib`。仓库忽略 `build/`，不提交生成文件。
