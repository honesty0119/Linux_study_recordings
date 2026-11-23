#!/usr/bin/env bash
# Ubuntu主机一键构建/打包：仅写本任务build目录，不安装软件、不改全局环境。
set -euo pipefail
TASK_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SDK_ROOT=${RK3568_SDK:-"/opt/rk3568-sdk"}
SDK_ROOT=$(realpath -- "$SDK_ROOT")
BUILD_DIR=$(realpath -m -- "${RK3568_BUILD_DIR:-$TASK_ROOT/build/qt_demo-rk3568-cli}")
case "$BUILD_DIR/" in "$TASK_ROOT/build/"*) ;; *) printf '构建目录必须位于本任务build目录内\n' >&2; exit 1;; esac
JOBS=${JOBS:-4}
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { printf 'JOBS必须为正整数\n' >&2; exit 1; }
for program in make file sha256sum tar; do command -v "$program" >/dev/null || { printf '缺少工具：%s\n' "$program" >&2; exit 1; }; done
QMAKE="$SDK_ROOT/bin/qmake"
CXX="$SDK_ROOT/bin/aarch64-buildroot-linux-gnu-g++"
[[ -x "$QMAKE" && -x "$CXX" ]] || { printf 'SDK工具不存在：%s\n' "$SDK_ROOT" >&2; exit 1; }
[[ "$("$CXX" -dumpmachine)" == aarch64-buildroot-linux-gnu ]] || { printf '目标编译器不匹配\n' >&2; exit 1; }
QT_VERSION=$("$QMAKE" -query QT_VERSION)
[[ "$QT_VERSION" == 5.15.2 ]] || { printf '当前任务已验证SDK为Qt5.15.2，实际为%s，请先核实\n' "$QT_VERSION" >&2; exit 1; }
mkdir -p -- "$BUILD_DIR"
cd -- "$BUILD_DIR"
export PATH="$SDK_ROOT/bin:$PATH"
# qmake的mkspec/sysroot均来自匹配厂商SDK，不使用Ubuntu桌面Qt库。
"$QMAKE" "$TASK_ROOT/qt_demo/qt_demo.pro" -spec devices/linux-buildroot-g++ CONFIG+=release CONFIG-=debug
make -j "$JOBS"
file qt_demo | tee file-info.txt
file qt_demo | grep -q 'ARM aarch64' || { printf '产物不是AArch64\n' >&2; exit 1; }
sha256sum qt_demo > SHA256SUMS
{
    printf 'task=03\nqt=%s\nsdk=%s\nsource=%s\nbuild_utc=%s\n' "$QT_VERSION" "$SDK_ROOT" "$TASK_ROOT/qt_demo" "$(date -u +%FT%TZ)"
    "$CXX" --version
} > build-info.txt
tar -czf qt_serial_assistant.tar.gz qt_demo SHA256SUMS build-info.txt
printf '\n构建完成：%s/qt_demo\n打包完成：%s/qt_serial_assistant.tar.gz\n' "$BUILD_DIR" "$BUILD_DIR"
printf '使用Windows scripts/deploy.py --build cli推送；本脚本不尝试在Ubuntu运行ARM程序。\n'