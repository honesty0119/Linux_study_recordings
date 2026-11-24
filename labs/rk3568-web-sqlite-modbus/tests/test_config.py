#!/usr/bin/env python3
"""管理接口集成：认证、Origin、版本冲突、事务/锁、重启加载及实际从站号。"""
import concurrent.futures,hashlib,json,os,secrets,socket,sqlite3,struct,subprocess,sys,time
from pathlib import Path
manager,status_binary,web_binary=map(lambda x:str(Path(x).resolve()),sys.argv[1:4]);root=Path(sys.argv[4]).resolve();root.mkdir(parents=True,exist_ok=True)
token=secrets.token_hex(32);token_file=root/'test-token';token_file.write_text(token+'\n');token_file.chmod(0o600)
db=root/'config.db';runtime=root/'runtime.state';admin_socket=root/'admin.sock';public_socket=root/'public.sock';origin='http://127.0.0.1:18081';passed=[];processes=[];files=[]
def exact(s,n):
    data=b''
    while len(data)<n:
        part=s.recv(n-len(data))
        if not part:raise EOFError('short response')
        data+=part
    return data
def start(name,args,marker):
    log=root/(name+'.log');f=log.open('w');files.append(f);proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT);processes.append(proc)
    for _ in range(100):
        if marker in log.read_text():return proc
        assert proc.poll() is None,log.read_text();time.sleep(.05)
    raise AssertionError('startup timeout '+name)
def stop(proc):
    proc.terminate();proc.wait(timeout=5);assert proc.returncode==0

def scgi(sock,method='GET',path='/admin/api/v1/config',body='',auth=True,request_origin='',ctype='',query='',extra=None):
    data=body.encode();pairs=[('CONTENT_LENGTH',str(len(data))),('SCGI','1'),('REQUEST_METHOD',method),('PATH_INFO',path),('QUERY_STRING',query)]
    if sock==admin_socket:pairs += [('HTTP_AUTHORIZATION','Bearer '+token if auth is True else auth if isinstance(auth,str) else ''),('HTTP_ORIGIN',request_origin),('CONTENT_TYPE',ctype)]
    if extra:pairs+=extra
    headers=b''.join(k.encode()+b'\0'+v.encode()+b'\0' for k,v in pairs)
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(4);s.connect(str(sock));s.sendall(str(len(headers)).encode()+b':'+headers+b','+data)
        head=b''
        while not head.endswith(b'\r\n\r\n'):head+=exact(s,1)
        h=dict(line.decode().split(': ',1) for line in head[:-4].split(b'\r\n'));raw=exact(s,int(h['Content-Length']))
        assert h['Cache-Control']=='no-store' and token.encode() not in raw
        return int(h['Status']),json.loads(raw)
def current():
    code,data=scgi(admin_socket);assert code==200;return data

def payload(config,**updates):
    c=dict(config);c.update(updates);return '&'.join(str(k)+'='+str(v) for k,v in c.items())
def post(body,**kw):return scgi(admin_socket,method='POST',body=body,request_origin=kw.pop('request_origin',origin),ctype=kw.pop('ctype','application/x-www-form-urlencoded'),**kw)
base_args=[manager,'--socket',str(admin_socket),'--config-db',str(db),'--runtime-file',str(runtime),'--token-file',str(token_file),'--origin',origin,'--duration','120']
try:
    mgmt=start('manager',base_args,'CONFIG_API_READY')
    first=current();assert first['saved']['revision']==1 and first['runtime'] is None;passed.append('initial_config_and_no_runtime')
    for auth in [False,'Bearer '+'0'*64]:assert scgi(admin_socket,auth=auth)[0]==401;passed.append('authentication_rejection')
    assert scgi(admin_socket,query='secret=x')[0]==400;passed.append('query_rejected')
    assert scgi(admin_socket,body='x')[0]==400;passed.append('GET_body_rejected')
    assert scgi(admin_socket,extra=[('SCGI','1')])[0]==400;passed.append('duplicate_SCGI_rejected')
    for value in ['', 'http://untrusted.invalid']:
        assert post(payload(first['saved']),request_origin=value)[0]==403;passed.append('origin_rejected')
    assert post(payload(first['saved']),ctype='text/plain')[0]==415;passed.append('content_type_rejected')
    assert post('a'*513)[0]==413;passed.append('body_bound')
    assert scgi(admin_socket,method='DELETE')[0]==405;passed.append('method_rejected')
    code,saved=post(payload(first['saved'],unit=7,baud=115200,parity='E',stop_bits=2,history_seconds=2))
    assert code==200 and saved['saved']['revision']==2 and saved['runtime'] is None and saved['serial_state']=='disabled_hardware_pending';passed.append('save_115200_and_revision')
    good=saved['saved']
    for key,value in [('unit',0),('unit',248),('baud',12345),('data_bits',7),('stop_bits',3),('history_seconds',0),('history_seconds',3601),('parity','X'),('revision',0),('revision','-1'),('unit','1%27')]:
        assert post(payload(good,**{key:value}))[0]==400;assert current()['saved']==good;passed.append('validation_'+key+'_'+str(value))
    for bad in [payload(good)+'&unit=2',payload(good)+'&unknown=1',payload(good).replace('&parity=E',''),payload(good)+'&']:
        assert post(bad)[0]==400 and current()['saved']==good;passed.append('strict_form')
    assert post(payload(good,revision=1))[0]==409;passed.append('stale_revision_rejected')
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:answers=list(pool.map(lambda unit:post(payload(good,unit=unit)),[8,9]))
    assert sorted(code for code,_ in answers)==[200,409];good=current()['saved'];assert good['revision']==3;passed.append('concurrent_compare_and_swap')
    with sqlite3.connect(db) as lock:
        lock.execute('BEGIN IMMEDIATE');assert post(payload(good,unit=10))[0]==503;lock.rollback()
    assert current()['saved']==good;passed.append('busy_rollback_preserves_config')
    with sqlite3.connect(db) as conn:assert conn.execute('select count(*) from audit').fetchone()[0]==2
    passed.append('only_successful_changes_audited')
    stop(mgmt);mgmt=start('manager-restart',base_args,'CONFIG_API_READY');assert current()['saved']==good;passed.append('manager_restart_persistence')
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    status_args=[status_binary,'--interface','lo','--bind','127.0.0.1','--port',str(port),'--duration','45','--config-db',str(db),'--runtime-file',str(runtime),'--db',str(root/'history.db')]
    live=start('live',status_args,'READY interface=');assert current()['service_state']=='loaded_at_start';passed.append('startup_load')
    public=start('public',[web_binary,'--socket',str(public_socket),'--db',str(root/'history.db'),'--target','127.0.0.1','--port',str(port),'--runtime-file',str(runtime),'--duration','60'],'WEB_API_READY')
    def public_status():return scgi(public_socket,path='/api/v1/status')
    code,live_data=public_status();assert code==200 and live_data['ipv4']=='127.0.0.1';passed.append('web_discovers_actual_unit')
    session=live_data['session']
    code,changed=post(payload(good,unit=17,history_seconds=1));assert code==200 and changed['service_state']=='pending_restart' and changed['runtime']['unit']==good['unit'];passed.append('saved_not_applied')
    assert public_status()[0]==200;passed.append('pending_config_does_not_break_live_web')
    with socket.create_connection(('127.0.0.1',port),timeout=4) as s:
        s.sendall(struct.pack('>HHHBBHH',81,0,6,good['unit'],6,0,123));head=exact(s,7);assert exact(s,struct.unpack('>HHHB',head)[2]-1)==b'\x86\x01'
    assert current()['saved']==changed['saved'];passed.append('Modbus_write_still_rejected')
    stop(live);assert not runtime.exists() and current()['runtime'] is None and public_status()[0]==503;passed.append('stopped_runtime_not_applied')
    live=start('live-restart',status_args,'READY interface=');assert current()['runtime']['unit']==17 and current()['service_state']=='loaded_at_start'
    code,new=public_status();assert code==200 and new['session']!=session;passed.append('task_restart_applies_and_web_tracks_unit')
    stop(live);runtime.write_text('v1 4 17 1 1 0 1\n');assert current()['runtime'] is None and public_status()[0]==503;runtime.unlink();passed.append('stale_runtime_rejected')
    stop(public);stop(mgmt)
    bad=root/'wrong.db'
    with sqlite3.connect(bad) as conn:conn.execute('CREATE TABLE foreign_data(x)');conn.commit()
    digest=hashlib.sha256(bad.read_bytes()).hexdigest()
    assert subprocess.run([manager,'--init-db',str(bad)],capture_output=True).returncode==1
    assert hashlib.sha256(bad.read_bytes()).hexdigest()==digest;passed.append('foreign_database_not_overwritten')
    for name,kind in [('public-token','mode'),('symlink-token','symlink')]:
        f=root/name
        if kind=='mode':f.write_text(token);f.chmod(0o644)
        else:f.symlink_to(token_file)
        args=base_args.copy();args[args.index('--token-file')+1]=str(f)
        assert subprocess.run(args,capture_output=True,timeout=5).returncode==1;passed.append('token_'+kind+'_rejected')
    assert all(token not in log.read_text() for log in root.glob('*.log'));passed.append('no_token_in_logs')
    result={'result':'PASS','count':len(passed),'checks':passed,'scope':'host Unix SCGI and loopback C integration; not board nginx/SSH/browser verification'}
    (root/'config-tests.json').write_text(json.dumps(result,indent=2)+'\n');print('HOST_CONFIG_PASS',len(passed))
finally:
    for proc in processes:
        if proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=5)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
    for f in files:f.close()
