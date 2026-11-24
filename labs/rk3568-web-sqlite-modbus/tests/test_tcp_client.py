#!/usr/bin/env python3
"""只读C主站集成测试：真实回环TCP、合成边界值和真实状态服务，绝不连接开发板。"""
import json,socket,struct,subprocess,sys,threading,time
from pathlib import Path
client=str(Path(sys.argv[1]).resolve());server=str(Path(sys.argv[2]).resolve())
out=Path(sys.argv[3]).resolve();out.mkdir(parents=True,exist_ok=True);checks=[]

def exact(s,n):
    data=b''
    while len(data)<n:
        chunk=s.recv(n-len(data))
        if not chunk:raise EOFError('short TCP request')
        data+=chunk
    return data

def query(s):
    tid,pid,n,unit=struct.unpack('>HHHB',exact(s,7))
    assert (pid,n,unit)==(0,6,7)
    assert exact(s,n-1)==bytes.fromhex('0300000040'),'client must only read full snapshot'
    return tid

def words():
    r=[0]*64;r[0]=0x0200;r[1]=255;r[2:4]=[0xffff,0xffff]
    r[4:8]=[0x1234,0x5678,0x9abc,0xdef0];r[18]=1
    r[24:28]=[0,0,1,0];r[28:32]=[0,1,0,0]
    r[32:36]=[0xffff]*4;r[36:40]=[0x20,0,0,1]
    r[40:42]=[0xc000,0x020a];r[42]=65513;r[43]=1
    r[46:48]=[1,0]
    return r

def reply(s,tid,r,mode):
    pdu=bytes([3,128])+struct.pack('>64H',*r)
    if mode=='exception':pdu=bytes([0x83,2])
    if mode=='short_count':pdu=bytes([3,126])+struct.pack('>63H',*r[:63])
    packet=struct.pack('>HHHB',(tid+1)&65535 if mode=='wrong_tid' else tid,1 if mode=='wrong_pid' else 0,len(pdu)+(2 if mode=='wrong_length' else 1),8 if mode=='wrong_unit' else 7)+pdu
    s.sendall(packet[:3]);time.sleep(.005);s.sendall(packet[3:9]);time.sleep(.005);s.sendall(packet[9:])

def run_client(port,count=1):
    return subprocess.run([client,'--host','127.0.0.1','--port',str(port),'--unit','7','--count',str(count),'--interval-ms','100'],capture_output=True,text=True,timeout=12)

def fixture(mode):
    errors=[]
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(2);listener.settimeout(6)
    port=listener.getsockname()[1]
    def serve():
        try:
            with listener:
                with listener.accept()[0] as s:
                    s.settimeout(3);tid=query(s);r=words()
                    if mode=='unknown':r[1]=0
                    if mode=='stale':r[16:18]=[0,4001];r[45]=1
                    if mode=='bad_version':r[0]=0x0300
                    if mode=='timeout':time.sleep(1.3);return
                    reply(s,tid,r,mode)
                    if mode=='reconnect':query(s) # 第二次完整读取请求到达后断开；第三轮应新建连接。
                if mode=='reconnect':
                    with listener.accept()[0] as s:
                        s.settimeout(3);reply(s,query(s),words(),'valid')
        except BaseException as error:errors.append(repr(error))
    thread=threading.Thread(target=serve,daemon=True);thread.start()
    try:result=run_client(port,3 if mode=='reconnect' else 1)
    finally:thread.join(timeout=7)
    assert not thread.is_alive() and not errors,(mode,errors)
    rows=[json.loads(line) for line in result.stdout.splitlines()]
    expected=0 if mode in ('valid','unknown','stale') else 1
    assert result.returncode==expected,(mode,result.returncode,result.stderr)
    if mode=='valid':
        r=rows[0];assert r['seq']==4294967295 and r['session']=='123456789abcdef0'
        assert r['rx_bytes']=='18446744073709551615' and r['tx_bytes']=='9007199254740993'
        assert r['rx_errors']=='65536' and r['tx_errors']=='4294967296'
        assert r['ipv4']=='192.0.2.10' and r['signal_dbm']==-23
        assert r['counter_epoch']==65536 and r['rtu_enabled'] is False and r['stale'] is False
    elif mode=='unknown':
        assert all(rows[0][key] is None for key in ('carrier','ipv4','signal_dbm','rx_bytes','tx_bytes','rx_errors','tx_errors','sample_realtime_ms'))
    elif mode=='stale':assert rows[0]['stale'] is True and rows[0]['age_ms']==4001
    elif mode=='reconnect':
        assert [row['poll'] for row in rows]==[1,3] and 'passed=2 failed=1' in result.stderr
    else:assert rows==[],(mode,rows)
    (out/(mode+'.json')).write_text(json.dumps({'exit':result.returncode,'rows':rows,'stderr':result.stderr},indent=2)+'\n')
    checks.append(mode)

for mode in ('valid','unknown','stale','bad_version','exception','wrong_tid','wrong_unit','wrong_pid','wrong_length','short_count','timeout','reconnect'):fixture(mode)
for args in (['--host','localhost'],['--host','127.0.0.1','--unit','0'],['--host','127.0.0.1','--count','0'],['--host','127.0.0.1','--interval-ms','3000']):
    result=subprocess.run([client]+args,capture_output=True,text=True,timeout=2)
    assert result.returncode==2;checks.append('invalid_args_'+str(len(checks)))
with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
log=out/'real-server.log'
with log.open('w') as f:
    proc=subprocess.Popen([server,'--interface','lo','--bind','127.0.0.1','--port',str(port),'--unit','7','--duration','15'],stdout=f,stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            assert proc.poll() is None,log.read_text()
            if 'READY' in log.read_text():break
            time.sleep(.05)
        else:raise AssertionError('server readiness timeout')
        result=run_client(port,3);rows=[json.loads(line) for line in result.stdout.splitlines()]
        assert result.returncode==0 and len(rows)==3,result.stderr
        assert all(r['ipv4']=='127.0.0.1' and r['rtu_enabled'] is False for r in rows)
        assert len({r['session'] for r in rows})==1
        checks.append('real_server_three_reads')
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
    assert proc.returncode==0,log.read_text()
(out/'client-tests.json').write_text(json.dumps({'result':'PASS','count':len(checks),'checks':checks,'scope':'host loopback; synthetic values are fixtures, not board measurements'},indent=2)+'\n')
print('HOST_TCP_CLIENT_PASS',len(checks))
