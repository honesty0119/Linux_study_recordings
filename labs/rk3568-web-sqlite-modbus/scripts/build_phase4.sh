#!/usr/bin/env bash
# 第四阶段：配置管理、鉴权、版本保护、启动加载与只读功能回归。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TC=${TASK5_TOOLCHAIN:?Set TASK5_TOOLCHAIN to the matching AArch64 SDK root}
DEPS=${TASK5_TARGET_MODBUS:?Set TASK5_TARGET_MODBUS to the AArch64 libmodbus prefix}
HOSTDEPS=${TASK5_HOST_MODBUS:?Set TASK5_HOST_MODBUS to the x86_64 libmodbus prefix}
for tool in gcc python3 tee; do command -v "$tool" >/dev/null || { echo "Missing $tool" >&2;exit 1; }; done
CC="$TC/bin/aarch64-buildroot-linux-gnu-gcc"
SYSROOT=$("$CC" -print-sysroot)
mkdir -p "$ROOT/build"
RUN=$(mktemp -d "$ROOT/build/phase4.XXXXXXXX")
exec > >(tee "$RUN/build.log") 2>&1
trap 'rc=$?; if ((rc)); then printf "PHASE4_FAILED exit=%s log=%s/build.log\n" "$rc" "$RUN"; fi' EXIT
printf 'RUN=%s\n' "$RUN"
mkdir -p "$RUN/source" "$RUN/host/include" "$RUN/board/bin" "$RUN/board/lib" "$RUN/board/licenses" "$RUN/board/web" "$RUN/board/scripts" "$RUN/tests"
cp "$ROOT/src/"{status_server.c,history.c,history.h,wifi_signal.c,wifi_signal.h,web_api.c,config_store.c,config_store.h,config_api.c} "$RUN/source/"
cp "$ROOT/tests/"{test_protocol.py,test_history.py,test_wifi_parser.c,test_web.py,test_connection_diagnostics.py,test_config.py} "$RUN/source/"
cp "$ROOT/scripts/"{make_web_config.py,prepare_config_runtime.py,build_phase4.sh} "$RUN/source/"
cp "$ROOT/web/"{index.html,app.js,style.css,admin.html,admin.js,admin.css} "$RUN/board/web/"
cp "$RUN/source/"{make_web_config.py,prepare_config_runtime.py} "$RUN/board/scripts/"
cp "$SYSROOT/usr/include/sqlite3.h" "$RUN/host/include/"
CFLAGS=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread)
SOURCES=("$RUN/source/status_server.c" "$RUN/source/history.c" "$RUN/source/wifi_signal.c" "$RUN/source/config_store.c")
gcc "${CFLAGS[@]}" -I"$HOSTDEPS/include/modbus" -I"$RUN/host/include" "${SOURCES[@]}" "$HOSTDEPS/lib/libmodbus.a" -Wl,-l:libsqlite3.so.0 -o "$RUN/host/status_server"
gcc "${CFLAGS[@]}" -I"$HOSTDEPS/include/modbus" -I"$RUN/host/include" "$RUN/source/web_api.c" "$RUN/source/config_store.c" "$HOSTDEPS/lib/libmodbus.a" -Wl,-l:libsqlite3.so.0 -o "$RUN/host/web_api"
gcc "${CFLAGS[@]}" -I"$RUN/source" "$RUN/source/test_wifi_parser.c" "$RUN/source/wifi_signal.c" -o "$RUN/host/test_wifi_parser"
"$RUN/host/test_wifi_parser"
gcc "${CFLAGS[@]}" -I"$RUN/host/include" "$RUN/source/config_api.c" "$RUN/source/config_store.c" -Wl,-l:libsqlite3.so.0 -o "$RUN/host/config_api"
python3 "$RUN/source/test_protocol.py" "$RUN/host/status_server" "$RUN/tests/protocol"
python3 "$RUN/source/test_history.py" "$RUN/host/status_server" "$RUN/tests/history"
python3 "$RUN/source/test_web.py" "$RUN/host/web_api" "$RUN/host/status_server" "$RUN/tests/web"
python3 "$RUN/source/test_connection_diagnostics.py" "$RUN/host/status_server" "$RUN/tests/diagnostics"
python3 "$RUN/source/test_config.py" "$RUN/host/config_api" "$RUN/host/status_server" "$RUN/host/web_api" "$RUN/tests/config"
"$CC" "${CFLAGS[@]}" -I"$DEPS/include/modbus" "${SOURCES[@]}" -L"$DEPS/lib" -Wl,-rpath,'$ORIGIN/../lib' -lmodbus -lsqlite3 -o "$RUN/board/bin/status_server"
"$CC" "${CFLAGS[@]}" -I"$DEPS/include/modbus" "$RUN/source/web_api.c" "$RUN/source/config_store.c" -L"$DEPS/lib" -Wl,-rpath,'$ORIGIN/../lib' -lmodbus -lsqlite3 -o "$RUN/board/bin/web_api"
"$CC" "${CFLAGS[@]}" "$RUN/source/config_api.c" "$RUN/source/config_store.c" -lsqlite3 -o "$RUN/board/bin/config_api"
cp "$DEPS/lib/libmodbus.so.5.1.0" "$RUN/board/lib/libmodbus.so.5"
cp "$ROOT/third_party/"{COPYING.LESSER,manifest.json} "$RUN/board/licenses/"
for name in status_server web_api config_api; do
    "$TC/bin/aarch64-buildroot-linux-gnu-readelf" -h "$RUN/board/bin/$name" > "$RUN/$name-elf.txt"
    grep -q AArch64 "$RUN/$name-elf.txt"
done
python3 "$RUN/source/make_web_config.py" --root "$RUN/board" --bind 127.0.0.1 --port 8080 --admin
python3 - "$RUN" <<'PYCODE'
import hashlib,json,sys
from pathlib import Path
run=Path(sys.argv[1]);paths=[p for folder in ['source','board'] for p in (run/folder).rglob('*') if p.is_file()]
(run/'artifact-hashes.json').write_text(json.dumps({str(p.relative_to(run)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
PYCODE
printf 'PHASE4_BUILD_PASS\n产物：%s\n仅主机回归与交叉编译通过；配置管理的板端部署/SSH隧道/nginx/浏览器验收待执行。\n' "$RUN"
