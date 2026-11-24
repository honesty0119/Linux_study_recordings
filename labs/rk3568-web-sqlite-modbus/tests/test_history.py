#!/usr/bin/env python3
"""本机SQLite集成：持久记录、进程重启、未知schema、锁冲突、降级TCP。"""
import json,socket,sqlite3,struct,subprocess,sys,time
from pathlib import Path
from test_protocol import frame,response
binary=str(Path(sys.argv[1]).resolve());root=Path(sys.argv[2]).resolve();root.mkdir(parents=True,exist_ok=True)
passed=[]

def start(label,db,duration=3):
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    path=root/(label+'.log');f=path.open('w')
    p=subprocess.Popen([binary,'--interface','lo','--bind','127.0.0.1','--port',str(port),'--duration',str(duration),'--db',str(db),'--history-interval','1'],stdout=f,stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            if 'READY' in path.read_text():return p,f,port
            if p.poll() is not None:raise AssertionError(path.read_text())
            time.sleep(.05)
        raise AssertionError('startup timeout')
    except BaseException:
        if p.poll() is None:p.terminate();p.wait(timeout=5)
        f.close();raise

def wait(p,f,expected):
    try:assert p.wait(timeout=10)==expected
    finally:
        if p.poll() is None:p.kill();p.wait()
        f.close()

def diagnostic(port):
    with socket.create_connection(('127.0.0.1',port),timeout=2) as s:
        s.sendall(frame(struct.pack('>BHH',3,45,1)));data=response(s)
        assert data[:2]==b'\x03\x02'
        return struct.unpack('>H',data[2:])[0]

db=root/'history.db'
p,f,port=start('first',db);wait(p,f,0)
with sqlite3.connect(db) as c:
    assert c.execute('PRAGMA integrity_check').fetchone()[0]=='ok'
    assert c.execute('PRAGMA user_version').fetchone()[0]==1
    assert c.execute('PRAGMA application_id').fetchone()[0]==1412771889
    assert c.execute('PRAGMA journal_mode').fetchone()[0]=='wal'
    count=c.execute('SELECT count(*) FROM samples').fetchone()[0];assert count>=2
    assert c.execute('SELECT count(*) FROM samples WHERE length(registers)!=128 OR signal_dbm IS NOT NULL').fetchone()[0]==0
    assert c.execute("SELECT count(*) FROM events WHERE kind='session_start'").fetchone()[0]==1
passed.append('persisted_schema_and_null_unknown')
p,f,port=start('second',db);wait(p,f,0)
with sqlite3.connect(db) as c:
    assert c.execute('SELECT count(*) FROM samples').fetchone()[0]>count
    assert c.execute('SELECT count(*) FROM sessions').fetchone()[0]==2
passed.append('process_restart_retains_history')
p,f,port=start('bad_path',root/'missing-parent'/'data.db',2)
try:assert diagnostic(port)&6==6
finally:wait(p,f,1)
passed.append('database_failure_keeps_tcp_live')
foreign=root/'foreign.db'
with sqlite3.connect(foreign) as c:c.executescript('CREATE TABLE keep(value);INSERT INTO keep VALUES(42);PRAGMA user_version=99;')
p,f,port=start('foreign_schema',foreign,2)
try:assert diagnostic(port)&2
finally:wait(p,f,1)
with sqlite3.connect(foreign) as c:
    assert c.execute('SELECT value FROM keep').fetchone()[0]==42
    assert c.execute('PRAGMA user_version').fetchone()[0]==99
passed.append('foreign_schema_not_overwritten')
p,f,port=start('locked',db,5)
try:
    time.sleep(.2)
    with sqlite3.connect(db,timeout=2) as c:
        c.execute('BEGIN IMMEDIATE');time.sleep(1.5)
        before=time.monotonic();flags=diagnostic(port);elapsed=time.monotonic()-before
        assert flags&4 and elapsed<1.5,(flags,elapsed)
        c.rollback()
    time.sleep(1.2)
    assert diagnostic(port)&4
finally:wait(p,f,1)
passed.append('busy_database_loss_visible_tcp_responsive')
unused=root/'once-must-not-create.db'
p=subprocess.run([binary,'--interface','lo','--once','--db',str(unused)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
assert p.returncode==2 and not unused.exists()
passed.append('once_does_not_write')
report={'result':'PASS','count':len(passed),'tests':passed,'scope':'host local filesystem and process restart; no board reboot or power loss test'}
(root/'history-tests.json').write_text(json.dumps(report,indent=2)+'\n');print('HOST_HISTORY_PASS',len(passed))
