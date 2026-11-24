#!/usr/bin/env bash
# 验证TCP启停工具；C程序复用已验证tcp.68WIDBHX，不重编译、不连接开发板。
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
BASE=${TASK5_TCP_BUILD:?Set TASK5_TCP_BUILD to the absolute build_tcp.sh result directory}
mkdir -p "$ROOT/build"
RUN=$(mktemp -d "$ROOT/build/ops.XXXXXXXX")
exec > >(tee "$RUN/check.log") 2>&1
trap 'rc=$?; if ((rc)); then printf "TCP_OPS_FAILED exit=%s log=%s/check.log\n" "$rc" "$RUN"; fi' EXIT
printf 'RUN=%s\n' "$RUN"
python3 - "$ROOT" "$BASE" <<'PYCODE'
import hashlib,json,sys
from pathlib import Path
root,base=map(Path,sys.argv[1:])
for name,digest in json.loads((base/'artifact-hashes.json').read_text()).items():
    assert hashlib.sha256((base/name).read_bytes()).hexdigest()==digest,name
for f in (base/'source').iterdir():
    if f.suffix in ('.c','.h'):assert f.read_bytes()==(root/'src'/f.name).read_bytes(),f.name
print('BASE_TCP_ARTIFACTS_VERIFIED')
PYCODE
mkdir -p "$RUN/source" "$RUN/t" "$RUN/board/scripts"
cp "$ROOT/scripts/tcp_service.py" "$ROOT/tests/"{test_tcp_service.py,tcp_soak.py,test_soak_harness.py} "$ROOT/scripts/check_tcp_ops.sh" "$RUN/source/"
cp -a "$BASE/board/bin" "$BASE/board/lib" "$BASE/board/licenses" "$RUN/board/"
cp "$RUN/source/tcp_service.py" "$RUN/board/scripts/"
python3 "$RUN/source/test_tcp_service.py" "$RUN/source/tcp_service.py" "$BASE/host/status_server" "$BASE/host/tcp_status_client" "$RUN/t"
python3 "$RUN/source/test_soak_harness.py" "$RUN/source/tcp_soak.py" "$RUN/soak"
python3 - "$RUN" <<'PYCODE'
import hashlib,json,sys
from pathlib import Path
run=Path(sys.argv[1])
(run/'artifact-hashes.json').write_text(json.dumps({str(p.relative_to(run)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ['source','board'] for p in (run/folder).rglob('*') if p.is_file()},indent=2)+'\n')
PYCODE
printf 'TCP_OPS_PASS\n产物：%s\n仅Ubuntu启停与测试工具验证通过；C程序复用已验证构建，板端管理器和30分钟测试待执行。\n' "$RUN"
