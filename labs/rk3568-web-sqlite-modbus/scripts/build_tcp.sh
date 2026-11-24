#!/usr/bin/env bash
# TCP专项：复用现有C服务端，新增只读C主站；只在本任务目录生成产物。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TC=${TASK5_TOOLCHAIN:?Set TASK5_TOOLCHAIN to the matching AArch64 SDK root}
DEPS=${TASK5_TARGET_MODBUS:?Set TASK5_TARGET_MODBUS to the AArch64 libmodbus prefix}
HOSTDEPS=${TASK5_HOST_MODBUS:?Set TASK5_HOST_MODBUS to the x86_64 libmodbus prefix}
for tool in gcc python3 tee; do command -v "$tool" >/dev/null || { echo "Missing $tool" >&2;exit 1; }; done
CC="$TC/bin/aarch64-buildroot-linux-gnu-gcc"
SYSROOT=$("$CC" -print-sysroot)
mkdir -p "$ROOT/build"
RUN=$(mktemp -d "$ROOT/build/tcp.XXXXXXXX")
exec > >(tee "$RUN/build.log") 2>&1
trap 'rc=$?; if ((rc)); then printf "TCP_BUILD_FAILED exit=%s log=%s/build.log\n" "$rc" "$RUN"; fi' EXIT
printf 'RUN=%s\n' "$RUN"
mkdir -p "$RUN/source" "$RUN/host/include" "$RUN/board/bin" "$RUN/board/lib" "$RUN/board/licenses" "$RUN/tests"
cp "$ROOT/src/"{status_server.c,history.c,history.h,wifi_signal.c,wifi_signal.h,config_store.c,config_store.h,tcp_status_client.c} "$RUN/source/"
cp "$ROOT/tests/"{test_protocol.py,test_connection_diagnostics.py,test_tcp_client.py} "$RUN/source/"
cp "$ROOT/scripts/build_tcp.sh" "$RUN/source/"
cp "$SYSROOT/usr/include/sqlite3.h" "$RUN/host/include/"
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread)
SOURCES=("$RUN/source/status_server.c" "$RUN/source/history.c" "$RUN/source/wifi_signal.c" "$RUN/source/config_store.c")
gcc "${CFLAGS[@]}" -I"$HOSTDEPS/include/modbus" -I"$RUN/host/include" "${SOURCES[@]}" "$HOSTDEPS/lib/libmodbus.a" -Wl,-l:libsqlite3.so.0 -o "$RUN/host/status_server"
gcc "${CFLAGS[@]}" -I"$HOSTDEPS/include/modbus" "$RUN/source/tcp_status_client.c" "$HOSTDEPS/lib/libmodbus.a" -o "$RUN/host/tcp_status_client"
python3 "$RUN/source/test_protocol.py" "$RUN/host/status_server" "$RUN/tests/protocol"
python3 "$RUN/source/test_connection_diagnostics.py" "$RUN/host/status_server" "$RUN/tests/diagnostics"
python3 "$RUN/source/test_tcp_client.py" "$RUN/host/tcp_status_client" "$RUN/host/status_server" "$RUN/tests/client"
"$CC" "${CFLAGS[@]}" -I"$DEPS/include/modbus" "${SOURCES[@]}" -L"$DEPS/lib" -Wl,-rpath,'$ORIGIN/../lib' -lmodbus -lsqlite3 -o "$RUN/board/bin/status_server"
"$CC" "${CFLAGS[@]}" -I"$DEPS/include/modbus" "$RUN/source/tcp_status_client.c" -L"$DEPS/lib" -Wl,-rpath,'$ORIGIN/../lib' -lmodbus -o "$RUN/board/bin/tcp_status_client"
cp "$DEPS/lib/libmodbus.so.5.1.0" "$RUN/board/lib/libmodbus.so.5"
cp "$ROOT/third_party/"{COPYING.LESSER,manifest.json} "$RUN/board/licenses/"
for name in status_server tcp_status_client; do
    "$TC/bin/aarch64-buildroot-linux-gnu-readelf" -h "$RUN/board/bin/$name" > "$RUN/$name-elf.txt"
    grep -q AArch64 "$RUN/$name-elf.txt"
done
python3 - "$RUN" <<'PYCODE'
import hashlib,json,sys
from pathlib import Path
run=Path(sys.argv[1])
(run/'artifact-hashes.json').write_text(json.dumps({str(p.relative_to(run)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ['source','board'] for p in (run/folder).rglob('*') if p.is_file()},indent=2)+'\n')
PYCODE
printf 'TCP_BUILD_PASS\n产物：%s\n仅TCP主机回归与AArch64构建通过；新增主站的板端实测待执行，RTU暂不实施。\n' "$RUN"
