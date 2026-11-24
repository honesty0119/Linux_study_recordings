#!/usr/bin/env python3
"""仅在明确指定的私有部署目录创建管理令牌/配置库；不启动服务，不输出令牌。"""
import argparse,os,secrets,stat,subprocess
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--root',required=True);a=parser.parse_args()
root=Path(a.root).resolve()
assert root.is_dir() and not (root.stat().st_mode&0o077),'deployment directory must already be private (0700)'
assert (root/'bin/config_api').is_file()
run=root/'run';run.mkdir(mode=0o700,exist_ok=True)
fd=os.open(str(run/'admin-token'),os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
with os.fdopen(fd,'w') as f:f.write(secrets.token_hex(32)+'\n')
subprocess.run([str(root/'bin/config_api'),'--init-db',str(root/'config.db')],check=True)
print('CONFIG_FILES_PREPARED token contents are not printed')
