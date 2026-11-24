#!/usr/bin/env python3
"""主机真实socket验证：区分连接关闭原因，并验证1秒轮询不被3秒空闲规则关闭。"""
import json,socket,struct,subprocess,sys,time
from pathlib import Path
binary=str(Path(sys.argv[1]).resolve());out=Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=True)
checks=[]
def exact(s,n):
    result=b''
    while len(result)<n:
        b=s.recv(n-len(result))
        if not b:raise EOFError('unexpected close')
        result+=b
    return result
def launch(name,duration):
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    log=out/(name+'.log');f=log.open('w')
    proc=subprocess.Popen([binary,'--interface','lo','--bind','127.0.0.1','--port',str(port),'--duration',str(duration),'--trace-connections','1'],stdout=f,stderr=subprocess.STDOUT)
    for _ in range(100):
        if 'READY' in log.read_text():return proc,f,log,port
        assert proc.poll() is None,log.read_text();time.sleep(.05)
    proc.terminate();proc.wait(timeout=5);f.close();raise RuntimeError('startup timeout')
def connect(port):return socket.create_connection(('127.0.0.1',port),timeout=5)
def logged(log,peer,reason):
    reasons=(reason,) if isinstance(reason,str) else reason
    for _ in range(100):
        rows=[line for line in log.read_text().splitlines() if line.startswith('TCP_CLOSE ') and f'peer_port={peer} ' in line and any(f'reason={item} ' in line for item in reasons)]
        if rows:return rows[-1]
        time.sleep(.02)
    raise AssertionError((peer,reason,log.read_text()))
def stop(proc,f):
    if proc.poll() is None:
        proc.terminate()
        try:proc.wait(timeout=5)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
    f.close()
proc,f,log,port=launch('diagnostic',60)
try:
    for reason,packet in [('peer_closed',None),('idle_timeout',b''),('header_timeout',b'\x00'),('payload_timeout',struct.pack('>HHHB',42,0,6,1)+b'\x03'),('invalid_mbap',struct.pack('>HHHB',43,1,2,1))]:
        with connect(port) as s:
            peer=s.getsockname()[1]
            if packet is not None:
                if packet:s.sendall(packet)
                assert s.recv(1)==b''
        row=logged(log,peer,reason)
        if reason=='payload_timeout':assert 'last_tid=42 ' in row
        checks.append(reason)
    with connect(port) as s:
        peer=s.getsockname()[1]
        for tid in range(6):
            s.sendall(struct.pack('>HHHBBHH',tid,0,6,1,3,0,1))
            h=exact(s,7);actual,pid,n,unit=struct.unpack('>HHHB',h)
            assert (actual,pid,unit)==(tid,0,1) and exact(s,n-1)==bytes.fromhex('03020200')
            if tid<5:time.sleep(1)
    row=logged(log,peer,'peer_closed');assert 'requests=6 ' in row and 'last_tid=5 ' in row;checks.append('one_hz_persistent_connection')
    # 一帧完整响应后用RST关闭；仍须按accept时的非零端口找到日志。
    with connect(port) as s:
        peer=s.getsockname()[1]
        s.sendall(struct.pack('>HHHBBHH',101,0,6,1,3,0,1))
        h=exact(s,7);exact(s,struct.unpack('>HHHB',h)[2]-1)
        s.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
    row=logged(log,peer,('receive_error','peer_closed'))
    assert 'requests=1 ' in row and 'last_tid=101 ' in row
    checks.append('reset_preserves_peer_identity')
    held=[connect(port) for _ in range(4)]
    try:
        with connect(port) as fifth:
            peer=fifth.getsockname()[1];assert fifth.recv(1)==b''
        logged(log,peer,'capacity_rejected');checks.append('capacity_rejected')
    finally:
        for s in held:s.close()
    time.sleep(.1)
    with connect(port) as s:
        peer=s.getsockname()[1]
        s.sendall(struct.pack('>HHHBBHH',99,0,6,1,3,0,1))
        h=exact(s,7);exact(s,struct.unpack('>HHHB',h)[2]-1)
        proc.terminate();proc.wait(timeout=5);assert s.recv(1)==b''
    assert proc.returncode==0 and 'SERVER_STOP reason=signal ' in log.read_text()
    logged(log,peer,'server_shutdown');checks.append('signal_shutdown')
finally:stop(proc,f)
proc,f,log,port=launch('duration',2)
try:
    with connect(port) as s:
        peer=s.getsockname()[1]
        s.sendall(struct.pack('>HHHBBHH',100,0,6,1,3,0,1))
        h=exact(s,7);exact(s,struct.unpack('>HHHB',h)[2]-1)
        assert s.recv(1)==b''
    proc.wait(timeout=5)
    assert proc.returncode==0 and 'SERVER_STOP reason=duration ' in log.read_text()
    logged(log,peer,'server_shutdown');checks.append('duration_shutdown')
finally:stop(proc,f)
(out/'diagnostic-tests.json').write_text(json.dumps({'result':'PASS','count':len(checks),'checks':checks},indent=2)+'\n')
print('HOST_DIAGNOSTICS_PASS',len(checks))
