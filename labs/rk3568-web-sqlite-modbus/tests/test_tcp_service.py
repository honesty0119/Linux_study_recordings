#!/usr/bin/env python3
"""TCP管理器Linux回环验收；仅测试目录和本轮进程，保留日志，不连接板端。"""
import json,os,socket,subprocess,sys,tempfile,time
from pathlib import Path
manager=str(Path(sys.argv[1]).resolve());binary=str(Path(sys.argv[2]).resolve());client=str(Path(sys.argv[3]).resolve())
out=Path(sys.argv[4]).resolve();out.mkdir(parents=True,exist_ok=True);checks=[];roots=[]
def root():
    r=Path(tempfile.mkdtemp(prefix='r.',dir=str(out)));roots.append(r);return r
def port():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
def args(r,action,p=1502,duration=30,exe=binary):
    return [sys.executable,manager,action,'--root',str(r),'--binary',exe,'--interface','lo','--bind','127.0.0.1','--port',str(p),'--unit','7','--duration',str(duration)]
def call(r,action,p=1502,duration=30,exe=binary,expected=0):
    result=subprocess.run(args(r,action,p,duration,exe),capture_output=True,text=True,timeout=20)
    assert result.returncode==expected,(action,result.stdout,result.stderr)
    return json.loads(result.stdout) if result.stdout else None
def until_stopped(r):
    for _ in range(100):
        result=call(r,'status')
        if result['state']=='stopped':return result
        time.sleep(.1)
    raise AssertionError('did not stop')
try:
    r=root();p=port();assert call(r,'status')['state']=='not_started';checks.append('not_started')
    started=call(r,'start',p);assert started['state']=='running' and started['rtu_enabled'] is False;checks.append('start_ready')
    command=Path('/proc/'+str(started['child_pid'])+'/cmdline').read_bytes()
    assert b'--db' not in command and b'--config-db' not in command;checks.append('tcp_only_no_database')
    result=subprocess.run([client,'--host','127.0.0.1','--port',str(p),'--unit','7','--count','2','--interval-ms','100'],capture_output=True,text=True,timeout=5)
    assert result.returncode==0 and len(result.stdout.splitlines())==2;checks.append('C_client_reads')
    call(r,'start',p,expected=1);assert call(r,'status')['manager_pid']==started['manager_pid'];checks.append('duplicate_start_rejected')
    idle=socket.create_connection(('127.0.0.1',p),timeout=2)
    result=call(r,'stop');assert result['state']=='stopped' and result['child_exit']==0;idle.settimeout(2)
    assert idle.recv(1)==b'';idle.close();checks.append('stop_closes_active_connections')
    assert not (r/'control.sock').exists();checks.append('socket_removed')
    assert call(r,'stop')['state']=='stopped';checks.append('idempotent_stop')
    call(r,'start',p,duration=2);result=until_stopped(r);assert result['child_exit']==0;checks.append('duration_auto_stop_and_restart')
    with socket.socket() as occupied:
        occupied.bind(('127.0.0.1',0));occupied.listen(1);conflict=occupied.getsockname()[1];r2=root()
        call(r2,'start',conflict,expected=1)
        with socket.create_connection(('127.0.0.1',conflict),timeout=2):
            peer,_=occupied.accept();peer.close()
        assert json.loads((r2/'latest.json').read_text())['state']=='failed'
    checks.append('port_conflict_does_not_stop_owner')
    r3=root();(r3/'latest.json').write_text(json.dumps({'state':'stopped','child_pid':os.getpid()}))
    assert call(r3,'stop')['child_pid']==os.getpid();checks.append('forged_pid_never_signalled')
    r4=root();(r4/'control.sock').write_text('do not delete');call(r4,'start',port(),expected=1)
    assert (r4/'control.sock').read_text()=='do not delete';checks.append('foreign_control_file_preserved')
    r5=root();fake=out/'log_fixture.py';fake.write_text('#!/usr/bin/env python3\nimport time\nprint("READY interface=lo",flush=True)\ntime.sleep(.3)\nprint("x"*2200000,flush=True)\ntime.sleep(.3)\n')
    fake.chmod(0o700) # 仅给本轮新建测试程序执行权限。
    call(r5,'start',port(),exe=str(fake));result=until_stopped(r5)
    assert result['log_bytes']==1048576 and result['log_discarded_bytes']>1000000
    assert (Path(result['run_dir'])/'server.log').stat().st_size==1048576;checks.append('bounded_log_and_pipe_drain')
    r6=root();p=port();a=subprocess.Popen(args(r6,'start',p),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    b=subprocess.Popen(args(r6,'start',p),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    ao,ae=a.communicate(timeout=20);bo,be=b.communicate(timeout=20)
    assert sorted([a.returncode,b.returncode])==[0,1],(ao,ae,bo,be)
    call(r6,'stop');checks.append('concurrent_start_single_owner')
finally:
    for r in roots:
        subprocess.run(args(r,'stop'),capture_output=True,text=True,timeout=20)
(out/'manager-tests.json').write_text(json.dumps({'result':'PASS','count':len(checks),'checks':checks,'scope':'host loopback only'},indent=2)+'\n')
print('HOST_TCP_MANAGER_PASS',len(checks))
