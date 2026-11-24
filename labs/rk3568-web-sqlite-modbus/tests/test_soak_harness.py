#!/usr/bin/env python3
"""验证稳定性工具会正确判定正常/坏响应/断线恢复；合成回环流量不是板端稳定性结果。"""
import json,socket,struct,subprocess,sys,threading
from pathlib import Path
script=str(Path(sys.argv[1]).resolve());out=Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=True)
checks=[]
for mode in ('good','wrong_unit','reconnect'):
    done=threading.Event();lock=threading.Lock();counter=[0];threads=[]
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(8);listener.settimeout(.1)
    port=listener.getsockname()[1]
    def handle(s):
        with s:
            s.settimeout(.2)
            try:
                while not done.is_set():
                    data=b''
                    while len(data)<12:
                        try:part=s.recv(12-len(data))
                        except socket.timeout:
                            if done.is_set():return
                            continue
                        if not part:return
                        data+=part
                    tid,pid,n,unit,fc,address,count=struct.unpack('>HHHBBHH',data)
                    assert (pid,n,unit,fc,address,count)==(0,6,7,3,0,64)
                    with lock:counter[0]+=1;seq=counter[0]
                    if mode=='reconnect' and seq==1:return
                    r=[0]*64;r[0]=512;r[3]=seq;r[7]=123;r[17]=50
                    payload=bytes([3,128])+struct.pack('>64H',*r)
                    s.sendall(struct.pack('>HHHB',tid,0,131,8 if mode=='wrong_unit' else 7)+payload)
            except (OSError,AssertionError):return
    def accept():
        while not done.is_set():
            try:s,_=listener.accept()
            except socket.timeout:continue
            except OSError:return
            t=threading.Thread(target=handle,args=(s,),daemon=True);threads.append(t);t.start()
    t=threading.Thread(target=accept,daemon=True);t.start()
    try:
        result=subprocess.run([sys.executable,script,'--host','127.0.0.1','--port',str(port),'--seconds','2','--clients','2','--interval-ms','200','--out',str(out/mode)],capture_output=True,text=True,timeout=10)
        report=json.loads((out/mode/'summary.json').read_text())
        assert result.returncode==(0 if mode=='good' else 1),(mode,result.stdout,result.stderr)
        assert report['result']==('PASS' if mode=='good' else 'FAIL')
        if mode=='reconnect':assert sum(c['errors'] for c in report['clients'])==1 and sum(c['successes'] for c in report['clients'])>0 and sum(c['connections'] for c in report['clients'])>2
        checks.append(mode)
    finally:
        done.set();listener.close();t.join(timeout=2)
        for thread in threads:thread.join(timeout=2)
(out/'harness-tests.json').write_text(json.dumps({'result':'PASS','checks':checks,'scope':'local synthetic peers only; not board soak'},indent=2)+'\n')
print('SOAK_HARNESS_PASS',len(checks))
