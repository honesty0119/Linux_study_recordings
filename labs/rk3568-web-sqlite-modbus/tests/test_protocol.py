#!/usr/bin/env python3
"""任务05协议集成测试：仅启动本机回环服务，不接触板端或串口。"""
import concurrent.futures
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

def exact(s, size):
    out=b''
    while len(out)<size:
        data=s.recv(size-len(out))
        if not data: raise EOFError('unexpected close')
        out+=data
    return out

def frame(pdu, tid=37, unit=1, pid=0):
    return struct.pack('>HHHB',tid,pid,len(pdu)+1,unit)+pdu

def response(s,tid=37):
    actual,pid,length,uid=struct.unpack('>HHHB',exact(s,7))
    assert (actual,pid,uid)==(tid,0,1)
    assert 2<=length<=254
    return exact(s,length-1)

def connect(port):
    s=socket.create_connection(('127.0.0.1',port),timeout=4);s.settimeout(4);return s

def exchange(port,pdu):
    with connect(port) as s:
        s.sendall(frame(pdu));result=response(s)
    time.sleep(.02)
    return result

def read(port,start=0,count=64):
    result=exchange(port,struct.pack('>BHH',3,start,count))
    assert result[:2]==bytes([3,2*count]),result.hex()
    return struct.unpack('>'+'H'*count,result[2:])

def closed(port,packet):
    with connect(port) as s:
        s.sendall(packet)
        try: assert s.recv(260)==b'', 'malformed request received reply'
        except ConnectionResetError: pass
    time.sleep(.02)

def run(binary,logdir):
    logdir.mkdir(parents=True,exist_ok=True)
    once=json.loads(subprocess.check_output([binary,'--interface','lo','--once'],text=True))
    assert once['interface']=='lo' and once['rtu_enabled'] is False
    assert once['valid_flags']&2 and once['ipv4']=='127.0.0.1'
    with socket.socket() as pick:
        pick.bind(('127.0.0.1',0));port=pick.getsockname()[1]
    passed=['real_loopback_collection']
    logfile=(logdir/'host-server.log').open('w')
    proc=subprocess.Popen([binary,'--interface','lo','--bind','127.0.0.1','--port',str(port),'--duration','45'],stdout=logfile,stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            assert proc.poll() is None,'server exited during startup'
            if 'READY' in (logdir/'host-server.log').read_text():break
            time.sleep(.05)
        else:raise AssertionError('server not ready')
        base=read(port)
        assert base[0]==0x0200 and base[40:42]==(0x7f00,1)
        assert base[19:24]==(0,0,0,0,0) and base[44]==0
        assert base[48:]==(0,)*16
        passed.append('full_snapshot_and_ipv4')
        assert read(port,63,1)==(0,);passed.append('last_register')
        for start,count,code in [(0,0,3),(0,126,3),(64,1,2),(63,2,2),(65535,1,2),(0,125,2)]:
            assert exchange(port,struct.pack('>BHH',3,start,count))==bytes([0x83,code])
            passed.append(f'range_{start}_{count}')
        for pdu in [b'\x03',b'\x03\0\0\0\1\0']:
            assert exchange(port,pdu)==b'\x83\x03';passed.append('read_bad_length_'+str(len(pdu)))
        writes=[bytes.fromhex(x) for x in ['050000ff00','0600001234','0f000000010101','1000000001021234','16000000001234','170000000100000001021234','0400000001','4100']]
        for pdu in writes:
            assert exchange(port,pdu)==bytes([pdu[0]|0x80,1])
            assert read(port,0,1)==(0x0200,) and read(port,19,5)==(0,0,0,0,0)
            passed.append('reject_fc_'+hex(pdu[0]))
        with connect(port) as s:
            data=frame(bytes.fromhex('0300000001'),123)
            for part in [data[:2],data[2:7],data[7:]]:s.sendall(part);time.sleep(.01)
            assert response(s,123)==bytes.fromhex('03020200')
        passed.append('tcp_fragmentation');time.sleep(.02)
        with connect(port) as s:
            s.sendall(frame(bytes.fromhex('0300000001'),1)+frame(bytes.fromhex('03003f0001'),2))
            assert response(s,1)==bytes.fromhex('03020200') and response(s,2)==bytes.fromhex('03020000')
        passed.append('tcp_coalescing_transaction_ids');time.sleep(.02)
        with connect(port) as s:
            s.sendall(frame(bytes.fromhex('0300000001'),9,2)+frame(bytes.fromhex('0300000001'),10))
            assert response(s,10)==bytes.fromhex('03020200')
        passed.append('wrong_unit_ignored');time.sleep(.02)
        for packet in [frame(b'\x03',pid=1),struct.pack('>HHHB',1,0,255,1),struct.pack('>HHHB',1,0,1,1)]:
            closed(port,packet);passed.append('bad_mbap_'+packet.hex())
        closed(port,b'\x00');passed.append('partial_header_deadline')
        closed(port,struct.pack('>HHHB',1,0,6,1)+b'\x03');passed.append('partial_payload_deadline')
        def parallel(index):
            with connect(port) as s:
                for j in range(10):
                    tid=index*100+j;s.sendall(frame(bytes.fromhex('0300000040'),tid));p=response(s,tid)
                    assert len(p)==130 and p[:4]==bytes.fromhex('03800200')
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:list(pool.map(parallel,range(4)))
        passed.append('four_clients_40_requests');time.sleep(.05)
        before=read(port);time.sleep(1.2);after=read(port)
        assert before[2:4]!=after[2:4] and before[4:8]==after[4:8]
        passed.append('sampler_advances_same_session')
        proc.terminate();proc.wait(timeout=5);assert proc.returncode==0;passed.append('graceful_shutdown')
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
        logfile.close()
    report={'result':'PASS','tests':passed,'count':len(passed),'scope':'host loopback only; not RK3568 or Wi-Fi hardware validation'}
    (logdir/'host-tests.json').write_text(json.dumps(report,indent=2)+'\n')
    print('HOST_PROTOCOL_PASS',len(passed))

if __name__=='__main__':run(str(Path(sys.argv[1]).resolve()),Path(sys.argv[2]))
