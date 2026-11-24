#!/usr/bin/env python3
"""Windows/Linux可用的只读TCP稳定性测试。默认30分钟、3客户端；不中途修改门槛。"""
import argparse,concurrent.futures,ipaddress,json,math,socket,struct,threading,time
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True);p.add_argument('--port',type=int,default=502);p.add_argument('--unit',type=int,default=7)
    p.add_argument('--seconds',type=int,default=1800);p.add_argument('--clients',type=int,default=3);p.add_argument('--interval-ms',type=int,default=1000)
    p.add_argument('--p95-limit-ms',type=int,default=500);p.add_argument('--age-limit-ms',type=int,default=3000);p.add_argument('--out',required=True)
    a=p.parse_args();ipaddress.IPv4Address(a.host)
    if not(1<=a.port<=65535 and 1<=a.unit<=247 and 2<=a.seconds<=7200 and 1<=a.clients<=4 and 200<=a.interval_ms<=2000 and 1<=a.p95_limit_ms<=10000 and 1<=a.age_limit_ms<=60000):p.error('argument out of range')
    out=Path(a.out);out.mkdir(parents=True,exist_ok=False)
    config=vars(a).copy();config['zero_errors_required']=True;config['minimum_scheduled_coverage']=.95
    (out/'config.json').write_text(json.dumps(config,indent=2)+'\n',encoding='utf-8')
    stop=threading.Event();started=time.monotonic();end=started+a.seconds
    def worker(index):
        s=None;seq=0;latencies=[];errors=0;attempts=0;max_age=0;connections=0;next_due=started
        with (out/('client-'+str(index)+'.jsonl')).open('w',encoding='utf-8') as log:
            try:
                while not stop.is_set() and time.monotonic()<end:
                    if stop.wait(max(0,next_due-time.monotonic())) or time.monotonic()>=end:break
                    begin=time.monotonic();row={'elapsed_seconds':round(begin-started,3),'client':index};attempts+=1;seq=(seq+1)&65535
                    deadline=begin+2
                    def exact(n):
                        data=b''
                        while len(data)<n:
                            remaining=deadline-time.monotonic()
                            if remaining<=0:raise TimeoutError('response deadline')
                            s.settimeout(remaining);part=s.recv(n-len(data))
                            if not part:raise EOFError('peer closed')
                            data+=part
                        return data
                    try:
                        if s is None:s=socket.create_connection((a.host,a.port),timeout=1);connections+=1
                        s.settimeout(max(.001,deadline-time.monotonic()))
                        s.sendall(struct.pack('>HHHBBHH',seq,0,6,a.unit,3,0,64))
                        tid,pid,n,unit=struct.unpack('>HHHB',exact(7))
                        if (tid,pid,n,unit)!=(seq,0,131,a.unit):raise ValueError('MBAP mismatch')
                        payload=exact(130)
                        if payload[:2]!=bytes([3,128]):raise ValueError('FC03/count mismatch')
                        r=struct.unpack('>64H',payload[2:]);age=(r[16]<<16)|r[17]
                        if r[0]!=512 or r[44]!=0:raise ValueError('map version or RTU state mismatch')
                        max_age=max(max_age,age)
                        if age>a.age_limit_ms or r[45]&1:raise ValueError('stale snapshot')
                        elapsed=(time.monotonic()-begin)*1000;latencies.append(elapsed)
                        row.update(ok=True,latency_ms=round(elapsed,3),age_ms=age,session=''.join('%04x'%x for x in r[4:8]),seq=(r[2]<<16)|r[3])
                    except (OSError,EOFError,ValueError) as error:
                        errors+=1;row.update(ok=False,error=type(error).__name__+': '+str(error))
                        if s is not None:s.close();s=None
                    log.write(json.dumps(row)+'\n');log.flush()
                    next_due+=a.interval_ms/1000
                    if next_due<time.monotonic():next_due=time.monotonic()+a.interval_ms/1000
            finally:
                if s is not None:s.close()
        return {'client':index,'attempts':attempts,'successes':len(latencies),'errors':errors,'connections':connections,'max_age_ms':max_age,'latencies':latencies}
    interrupted=False
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.clients) as pool:
        futures=[pool.submit(worker,i) for i in range(a.clients)]
        try:results=[f.result() for f in futures]
        except KeyboardInterrupt:
            interrupted=True;stop.set();results=[f.result() for f in futures]
    elapsed=time.monotonic()-started;latencies=sorted(x for r in results for x in r.pop('latencies'))
    def percentile(p):return latencies[max(0,math.ceil(len(latencies)*p)-1)] if latencies else None
    minimum=math.floor(a.seconds/(a.interval_ms/1000)*.95)
    passed=not interrupted and elapsed>=a.seconds-.1 and all(r['errors']==0 and r['successes']>=minimum for r in results) and bool(latencies) and percentile(.95)<=a.p95_limit_ms
    report={'result':'PASS' if passed else 'FAIL','elapsed_seconds':round(elapsed,3),'interrupted':interrupted,'clients':results,'latency_ms':{'p50':percentile(.5),'p95':percentile(.95),'p99':percentile(.99),'max':max(latencies) if latencies else None},'scope':'TCP reads only; no RTU, writes, database or network changes','config':config}
    (out/'summary.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('TCP_SOAK_'+report['result'],sum(r['successes'] for r in results),'reads',sum(r['errors'] for r in results),'errors')
    return 0 if passed else 1
if __name__=='__main__':raise SystemExit(main())
