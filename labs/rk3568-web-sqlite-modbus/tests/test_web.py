#!/usr/bin/env python3
"""Unix SCGI端到端测试：真实实时服务、历史游标、拒写、坏帧与源失联。"""
import hashlib,json,socket,subprocess,sys,time
from pathlib import Path
api_binary,status_binary=map(lambda p:str(Path(p).resolve()),sys.argv[1:3]);root=Path(sys.argv[3]).resolve();root.mkdir(parents=True,exist_ok=True)
sockpath=root/'api.sock';db=root/'history.db';passed=[];processes=[];files=[]

def start(name,args,marker):
    log=root/(name+'.log');f=log.open('w');files.append(f)
    p=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT);processes.append(p)
    for _ in range(100):
        if marker in log.read_text():return p
        if p.poll() is not None:raise AssertionError(log.read_text())
        time.sleep(.05)
    raise AssertionError('startup timeout')

def call(path,query='',method='GET',raw=None):
    if raw is None:
        pairs=[('CONTENT_LENGTH','0'),('SCGI','1'),('REQUEST_METHOD',method),('PATH_INFO',path),('QUERY_STRING',query)]
        body=b''.join(k.encode()+b'\0'+v.encode()+b'\0' for k,v in pairs);raw=str(len(body)).encode()+b':'+body+b','
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(4);s.connect(str(sockpath));s.sendall(raw);headers=b''
        while not headers.endswith(b'\r\n\r\n'):
            chunk=s.recv(1)
            if not chunk:raise EOFError('no response headers')
            headers+=chunk
        h=dict(line.decode().split(': ',1) for line in headers[:-4].split(b'\r\n'))
        body=b'';length=int(h['Content-Length'])
        while len(body)<length:
            chunk=s.recv(length-len(body))
            if not chunk:raise EOFError('short response body')
            body+=chunk
    assert h['Cache-Control']=='no-store'
    return int(h['Status']),json.loads(body)

try:
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    status=start('status',[status_binary,'--interface','lo','--bind','127.0.0.1','--port',str(port),'--db',str(db),'--history-interval','1','--duration','45'],'READY')
    backend=start('api',[api_binary,'--socket',str(sockpath),'--db',str(db),'--target','127.0.0.1','--port',str(port),'--duration','45'],'WEB_API_READY')
    code,data=call('/api/v1/status');assert code==200 and data['source']=='live_modbus' and data['ipv4']=='127.0.0.1' and data['signal_dbm'] is None
    assert isinstance(data['rx_bytes'],str) and data['rtu_enabled'] is False;passed.append('live_status_and_64bit_strings')
    for route in ['/api/v1/status','/api/v1/history','/api/v1/events']:
        assert call(route,method='POST')[0]==405;passed.append('reject_POST_'+route)
    assert call('/missing')[0]==404;passed.append('unknown_route')
    for q in ['limit=0','limit=101','before=-1','limit=1&limit=2','bad=1','before=1%20OR%201','before=9999999999999999999999','&limit=1','limit=1&']:
        assert call('/api/v1/history',q)[0]==400;passed.append('query_'+q)
    assert call('/api/v1/status','limit=1')[0]==400;passed.append('live_query_rejected')
    for raw in [b'04097:',b'5000:',b'0:,']:
        assert call('',raw=raw)[0]==400;passed.append('malformed_scgi_'+raw.decode())
    time.sleep(2.2);status.terminate();status.wait(timeout=5);assert status.returncode==0
    digest=hashlib.sha256(db.read_bytes()).hexdigest()
    code,data=call('/api/v1/status');assert code==503 and data['error']=='source_unavailable';passed.append('live_failure_not_history_fallback')
    ids=[];cursor=None
    for _ in range(20):
        code,data=call('/api/v1/history','limit=1'+('&before='+cursor if cursor else ''))
        assert code==200 and data['source']=='sqlite_history'
        ids.extend(row['id'] for row in data['items'])
        assert all(row['signal_dbm'] is None for row in data['items'])
        if not data['has_more']:break
        cursor=data['next_cursor']
    assert len(ids)>=3 and len(ids)==len(set(ids)) and list(map(int,ids))==sorted(map(int,ids),reverse=True)
    passed.append('history_pagination_no_duplicates')
    code,data=call('/api/v1/events','limit=20');assert code==200 and data['items'][0]['kind']=='session_start';passed.append('events_query')
    assert hashlib.sha256(db.read_bytes()).hexdigest()==digest;passed.append('database_unchanged_by_reads')
    backend.terminate();backend.wait(timeout=5);assert backend.returncode==0 and not sockpath.exists();passed.append('socket_cleanup')
    report={'result':'PASS','count':len(passed),'tests':passed,'scope':'host SCGI/API integration, not nginx HTTP/browser or RK3568 validation'}
    (root/'web-tests.json').write_text(json.dumps(report,indent=2)+'\n');print('HOST_WEB_PASS',len(passed))
finally:
    for p in processes:
        if p.poll() is None:
            p.terminate()
            try:p.wait(timeout=5)
            except subprocess.TimeoutExpired:p.kill();p.wait()
    for f in files:f.close()
