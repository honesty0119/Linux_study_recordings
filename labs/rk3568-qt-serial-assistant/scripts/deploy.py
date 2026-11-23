"""Windows 一键部署：复用已验证的 SSH 配置，不改执行策略、不启动 GUI/串口。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime
from uuid import uuid4

def sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''): digest.update(block)
    return digest.hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',choices=['creator','cli'],default='creator')
    parser.add_argument('--ssh-config',type=Path,required=True,help='Path to your SSH config containing host rk3568-board')
    parser.add_argument('--plan',action='store_true',help='只检查本地输入并展示计划，不联网、不上传')
    args=parser.parse_args()
    task=Path(__file__).resolve().parent.parent
    build='qt_demo-rk3568-cli' if args.build=='cli' else 'qt_demo-rk3568-release'
    artifact=task/'build'/build/'qt_demo'
    if not artifact.is_file(): raise RuntimeError(f'请先编译：{artifact}')
    if not args.ssh_config.is_file(): raise RuntimeError('固定SSH配置不存在')
    with artifact.open('rb') as stream: header=stream.read(20)
    if len(header)!=20 or header[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(header[18:20],'little')!=183:
        raise RuntimeError('产物不是ELF64 little-endian AArch64')
    digest=sha256(artifact)
    if args.plan:
        print(json.dumps({'artifact':str(artifact),'sha256':digest,'host':'rk3568-board',
            'steps':['create isolated /tmp directory','upload and verify SHA256','check ldd','run non-serial self-test','upload test fixture'],
            'gui_started':False,'serial_opened':False},ensure_ascii=False,indent=2)); return 0
    openssh=Path(os.environ.get('WINDIR',r'C:\Windows'))/'System32/OpenSSH'
    ssh=openssh/'ssh.exe'; scp=openssh/'scp.exe'
    if not ssh.is_file() or not scp.is_file(): raise RuntimeError('Windows OpenSSH客户端缺失')
    evidence=task/'build/deployment'/('script-'+datetime.now().strftime('%Y%m%d-%H%M%S')+'-'+uuid4().hex[:8])
    evidence.mkdir(parents=True)
    common=['-F',str(args.ssh_config),'-o','BatchMode=yes','-o','StrictHostKeyChecking=yes']
    def run(argv,log,timeout=90):
        result=subprocess.run([str(x) for x in argv],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
        (evidence/log).write_bytes(result.stdout)
        if result.returncode: raise RuntimeError(f'命令失败，退出码{result.returncode}；见{evidence/log}')
        return result.stdout.decode('utf-8',errors='replace').strip()
    def board(command,log,timeout=90): return run([ssh,*common,'rk3568-board',command],log,timeout)
    remote=board('mktemp -d /tmp/task3-serial-package.XXXXXX','create-directory.log')
    if not re.fullmatch(r'/tmp/task3-serial-package\.[A-Za-z0-9]+',remote): raise RuntimeError('远端目录返回异常，停止上传')
    run([scp,'-O',*common,artifact,f'rk3568-board:{remote}/qt_demo'],'upload.log')
    remote_hash=board(f"chmod u+x '{remote}/qt_demo' && sha256sum '{remote}/qt_demo'",'integrity.log').split()[0]
    if remote_hash.lower()!=digest: raise RuntimeError('产物SHA256不匹配')
    dependencies=board(f"ldd '{remote}/qt_demo'",'dependencies.log')
    if 'not found' in dependencies: raise RuntimeError('目标依赖缺失')
    tests=board(f"exec '{remote}/qt_demo' --self-test",'self-test.log')
    for name in ['SERIAL_DATA_SELF_TEST','FILE_IO_SELF_TEST']:
        if not re.search(rf'{name}\s+passed=\s*\d+\s+failed=\s*0\b',tests):
            raise RuntimeError(f'{name}缺失或未通过；请确认编译的是选做功能新版')
    if re.search(r'(^|\s)FAIL\s',tests): raise RuntimeError('自检存在失败项')
    fixture=task/'tests/fixtures/all-bytes-4096.bin'
    if fixture.is_file():
        board(f"mkdir -p '{remote}/data/send'",'prepare-send-directory.log')
        run([scp,'-O',*common,fixture,f'rk3568-board:{remote}/data/send/all-bytes-4096.bin'],'fixture-upload.log')
        fixture_hash=board(f"sha256sum '{remote}/data/send/all-bytes-4096.bin'",'fixture-integrity.log').split()[0]
        if fixture_hash.lower()!=sha256(fixture): raise RuntimeError('测试文件SHA256不匹配')
    (evidence/'deployment.json').write_text(json.dumps({'date':datetime.now().astimezone().isoformat(),
        'source':str(artifact),'sha256':digest,'remote_file':remote+'/qt_demo','self_test_exit':0,
        'gui_started':False,'serial_opened':False},ensure_ascii=False,indent=2),encoding='utf-8')
    print(f'部署及自检通过：{remote}/qt_demo\n证据：{evidence}\n未启动GUI；/tmp文件重启可能丢失，接收日志请及时取回。')
    return 0

if __name__=='__main__':
    try: sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError) as error:
        print(f'部署中止：{error}',file=sys.stderr); sys.exit(1)