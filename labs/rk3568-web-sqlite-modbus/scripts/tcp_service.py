#!/usr/bin/env python3
"""TCP服务启停：私有Unix控制接口、flock单实例；只终止本管理器的直接子进程。
业务仍由C status_server实现。本工具不启动DB/Web/RTU，不修改网络或系统服务。
"""
import argparse,fcntl,ipaddress,json,os,selectors,signal,socket,stat,subprocess,sys,tempfile,time
from pathlib import Path
LOG_LIMIT=1024*1024

def private_root(value):
    p=Path(value)
    if not p.is_absolute() or p.resolve()!=p:raise ValueError('root must be absolute and canonical')
    st=p.stat()
    if not stat.S_ISDIR(st.st_mode) or st.st_uid!=os.getuid() or stat.S_IMODE(st.st_mode)!=0o700:
        raise ValueError('root must be an existing private directory owned by this user (0700)')
    if len(str(p/'control.sock').encode())>107:raise ValueError('Unix socket path too long')
    return p

def guard(root):
    fd=os.open(str(root/'manager.lock'),os.O_CREAT|os.O_RDWR|os.O_NOFOLLOW,0o600)
    st=os.fstat(fd)
    if not stat.S_ISREG(st.st_mode) or st.st_uid!=os.getuid() or st.st_nlink!=1:
        os.close(fd);raise ValueError('unsafe lock file')
    return fd

def busy(root):
    fd=guard(root)
    try:
        try:fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);return False
        except BlockingIOError:return True
    finally:os.close(fd)

def publish(root,value):
    fd,name=tempfile.mkstemp(prefix='.state.',dir=str(root))
    try:
        with os.fdopen(fd,'w') as f:json.dump(value,f);f.write('\n')
        os.replace(name,str(root/'latest.json'))
    finally:
        if os.path.exists(name):os.unlink(name)

def control(root,command):
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
        s.settimeout(2);s.connect(str(root/'control.sock'));s.sendall((command+'\n').encode())
        data=b''
        while not data.endswith(b'\n'):
            part=s.recv(4096)
            if not part:raise RuntimeError('control connection closed')
            data+=part
            if len(data)>8192:raise RuntimeError('control response too large')
    return json.loads(data)

def status(root):
    if busy(root):
        try:return control(root,'status')
        except (OSError,RuntimeError):
            if busy(root):return {'state':'unavailable','detail':'manager lock held; control unavailable'}
    if (root/'latest.json').exists():
        value=json.loads((root/'latest.json').read_text())
        if value.get('state') in ('stopped','failed'):return value
        return {'state':'unavailable','detail':'unclean manager exit; child may remain until its duration expires'}
    return {'state':'not_started'}

def supervise(a,root):
    lock=guard(root)
    try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:os.close(lock);raise RuntimeError('manager already active')
    child=None;listener=None;sel=selectors.DefaultSelector();log=None;owned_socket=False
    state={'state':'starting','manager_pid':os.getpid(),'bind':a.bind,'port':a.port,'unit':a.unit,'duration':a.duration,'rtu_enabled':False}
    reason='startup_failure';received=0;written=0;ready=False;interrupted=[False];started=time.monotonic();metrics=None;next_metric=started
    try:
        signal.signal(signal.SIGTERM,lambda *_:interrupted.__setitem__(0,True))
        signal.signal(signal.SIGINT,lambda *_:interrupted.__setitem__(0,True))
        control_path=root/'control.sock'
        if control_path.exists() or control_path.is_symlink():
            st=control_path.lstat()
            if not stat.S_ISSOCK(st.st_mode) or st.st_uid!=os.getuid():raise ValueError('unsafe control path')
            control_path.unlink() # 只有持有独占锁时清除上次崩溃留下的本用户socket。
        run=Path(tempfile.mkdtemp(prefix='run.',dir=str(root)));state['run_dir']=str(run)
        log=(run/'server.log').open('wb');metrics=(run/'resources.jsonl').open('w')
        listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);listener.bind(str(control_path));owned_socket=True
        listener.listen(4);listener.setblocking(False);sel.register(listener,selectors.EVENT_READ,'control')
        args=[a.binary,'--interface',a.interface,'--bind',a.bind,'--port',str(a.port),'--unit',str(a.unit),'--duration',str(a.duration),'--trace-connections','1']
        child=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,close_fds=True)
        state['child_pid']=child.pid # 仅展示，不用于另一个进程的kill操作。
        os.set_blocking(child.stdout.fileno(),False);sel.register(child.stdout,selectors.EVENT_READ,'log')
        publish(root,state);prefix=b'';reason='service_exit'
        while True:
            for key,_ in sel.select(.1):
                if key.data=='log':
                    chunk=os.read(child.stdout.fileno(),65536)
                    if not chunk:sel.unregister(child.stdout);continue
                    received+=len(chunk);take=chunk[:max(0,LOG_LIMIT-written)];log.write(take);written+=len(take);log.flush()
                    if not ready:
                        prefix=(prefix+chunk)[-8192:]
                        if b'READY interface=' in prefix:
                            ready=True;state['state']='running';publish(root,state)
                else:
                    conn,_=listener.accept()
                    with conn:
                        conn.settimeout(.2)
                        try:
                            command=conn.recv(32).strip()
                            reply=dict(state)
                            if command==b'stop':reason='requested_stop';interrupted[0]=True;reply['state']='stopping'
                            elif command!=b'status':reply={'state':'error','detail':'unknown command'}
                            conn.sendall((json.dumps(reply)+'\n').encode())
                        except (OSError,ValueError):pass
            if child.poll() is None and time.monotonic()>=next_metric:
                record={'elapsed_seconds':round(time.monotonic()-started,3)}
                try:
                    proc=Path('/proc')/str(child.pid)
                    fields=(proc/'stat').read_text().rsplit(')',1)[1].split()
                    record['cpu_seconds']=(int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK')
                    for row in (proc/'status').read_text().splitlines():
                        if row.startswith('VmRSS:'):record['rss_kib']=int(row.split()[1])
                    record['fd_count']=len(list((proc/'fd').iterdir()))
                except (OSError,ValueError,IndexError):record['unavailable']=True
                metrics.write(json.dumps(record)+'\n');metrics.flush();next_metric=time.monotonic()+10
            if interrupted[0]:
                if reason!='requested_stop':reason='manager_signal'
                break
            if child.poll() is not None:break
            if not ready and time.monotonic()-started>8:reason='readiness_timeout';break
            if time.monotonic()-started>a.duration+5:reason='manager_deadline';break
    finally:
        if child is not None:
            if child.poll() is None:
                child.terminate()
                try:child.wait(timeout=5)
                except subprocess.TimeoutExpired:child.kill();child.wait();reason='forced_stop'
            # 子进程退出后读完管道；日志达到限额后仍排空，避免反压阻塞服务。
            if child.stdout:
                while True:
                    chunk=child.stdout.read(65536)
                    if not chunk:break
                    received+=len(chunk);take=chunk[:max(0,LOG_LIMIT-written)];log.write(take);written+=len(take)
                child.stdout.close()
            state['child_exit']=child.returncode
        if log:log.close()
        if metrics:metrics.close()
        if listener:listener.close()
        sel.close()
        if owned_socket:(root/'control.sock').unlink()
        state.update(state='stopped' if ready and child is not None and child.returncode==0 and reason not in ('forced_stop','readiness_timeout','manager_deadline') else 'failed',reason=reason,elapsed_seconds=round(time.monotonic()-started,3),log_bytes=written,log_discarded_bytes=received-written)
        publish(root,state)
        if 'run_dir' in state:(Path(state['run_dir'])/'result.json').write_text(json.dumps(state,indent=2)+'\n')
        os.close(lock)
    return 0 if state['state']=='stopped' else 1

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['start','serve','status','stop']);parser.add_argument('--root',required=True)
    parser.add_argument('--binary');parser.add_argument('--interface',default='wlan0');parser.add_argument('--bind',default='127.0.0.1')
    parser.add_argument('--port',type=int,default=1502);parser.add_argument('--unit',type=int,default=1);parser.add_argument('--duration',type=int,default=300)
    a=parser.parse_args();root=private_root(a.root)
    if a.action in ('start','serve'):
        if not a.binary:raise ValueError('--binary required')
        binary=Path(a.binary)
        if not binary.is_absolute() or not binary.is_file():raise ValueError('absolute existing binary required')
        a.binary=str(binary.resolve());ipaddress.IPv4Address(a.bind)
        if not 1<=a.port<=65535 or not 1<=a.unit<=247 or not 1<=a.duration<=7200:raise ValueError('port/unit/duration out of range')
        if not a.interface or len(a.interface)>=16 or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-' for c in a.interface):raise ValueError('invalid interface')
    if a.action=='serve':return supervise(a,root)
    if a.action=='start':
        if busy(root):raise RuntimeError('manager already active')
        # 启动失败只终止此Popen对象，绝不根据latest.json中的PID发信号。
        args=[sys.executable,str(Path(__file__).resolve()),'serve','--root',str(root),'--binary',a.binary,'--interface',a.interface,'--bind',a.bind,'--port',str(a.port),'--unit',str(a.unit),'--duration',str(a.duration)]
        with tempfile.TemporaryFile() as boot:
            proc=subprocess.Popen(args,stdin=subprocess.DEVNULL,stdout=boot,stderr=boot,start_new_session=True,close_fds=True)
            for _ in range(100):
                if proc.poll() is not None:raise RuntimeError('manager exited during startup; inspect latest.json/server.log')
                try:
                    value=control(root,'status')
                    if value.get('state')=='running' and value.get('manager_pid')==proc.pid:print(json.dumps(value));return 0
                except (OSError,RuntimeError):pass
                time.sleep(.1)
            proc.terminate()
            try:proc.wait(timeout=8)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
            raise RuntimeError('manager startup timeout')
    if a.action=='stop' and busy(root):
        control(root,'stop')
        for _ in range(100):
            if not busy(root):break
            time.sleep(.1)
        else:raise RuntimeError('stop timeout; no PID-based fallback was attempted')
    value=status(root);print(json.dumps(value))
    return 0 if value.get('state') in ('running','stopped','not_started') else 1

if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,ValueError) as e:print('TCP_MANAGER_ERROR '+str(e),file=sys.stderr);sys.exit(1)
