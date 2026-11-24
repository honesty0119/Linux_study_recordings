#!/usr/bin/env python3
"""为任务私有nginx实例生成配置，不读取/修改/etc/nginx配置，不启动服务。"""
import argparse,ipaddress,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--root',required=True);p.add_argument('--bind',default='127.0.0.1');p.add_argument('--port',type=int,default=8080);p.add_argument('--admin',action='store_true');p.add_argument('--admin-port',type=int,default=8081);a=p.parse_args()
ipaddress.IPv4Address(a.bind)
assert 1<=a.port<=65535
root=Path(a.root).resolve();assert re.fullmatch(r'/[A-Za-z0-9_./-]+',str(root))
assert (root/'web/index.html').is_file()
for name in ['logs','run','tmp/client','tmp/proxy','tmp/fastcgi','tmp/uwsgi','tmp/scgi']:(root/name).mkdir(parents=True,exist_ok=True)
config="""daemon off;
master_process off;
pid {root}/run/nginx.pid;
lock_file {root}/run/nginx.lock;
error_log {root}/logs/nginx-error.log warn;
events {{ worker_connections 64; }}
http {{
    access_log off;
    server_tokens off;
    client_max_body_size 1k;
    client_header_timeout 5s;
    client_body_timeout 5s;
    send_timeout 5s;
    keepalive_timeout 5s;
    client_body_temp_path {root}/tmp/client;
    proxy_temp_path {root}/tmp/proxy;
    fastcgi_temp_path {root}/tmp/fastcgi;
    uwsgi_temp_path {root}/tmp/uwsgi;
    scgi_temp_path {root}/tmp/scgi;
    types {{ text/html html; text/css css; application/javascript js; }}
    default_type application/octet-stream;
    limit_req_zone $binary_remote_addr zone=task5:1m rate=5r/s;
    server {{
        listen {address}:{port};
        root {root}/web;
        add_header X-Content-Type-Options nosniff always;
        add_header X-Frame-Options DENY always;
        add_header Content-Security-Policy "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; frame-ancestors 'none'" always;
        if ($request_method != GET) {{ return 405; }}
        location = / {{ try_files /index.html =404; }}
        location = /index.html {{ try_files $uri =404; }}
        location = /app.js {{ try_files $uri =404; }}
        location = /style.css {{ try_files $uri =404; }}
        location ^~ /api/ {{
            limit_req zone=task5 burst=10 nodelay;
            scgi_param SCGI 1;
            scgi_param REQUEST_METHOD $request_method;
            scgi_param PATH_INFO $uri;
            scgi_param QUERY_STRING $query_string;
            scgi_pass_request_headers off;
            scgi_pass_request_body off;
            scgi_read_timeout 3s;
            scgi_send_timeout 2s;
            scgi_buffering off;
            scgi_pass unix:{root}/run/api.sock;
        }}
        location / {{ return 404; }}
    }}
}}
"""
rendered=config.format(root=root,address=a.bind,port=a.port)
if a.admin:
    assert 1<=a.admin_port<=65535 and a.admin_port!=a.port
    assert (root/'web/admin.html').is_file()
    admin="""
    limit_req_zone $binary_remote_addr zone=task5admin:1m rate=2r/s;
    server {{
        listen 127.0.0.1:{port};
        root {root}/web;
        client_max_body_size 512;
        add_header X-Content-Type-Options nosniff always;
        add_header X-Frame-Options DENY always;
        add_header Cache-Control no-store always;
        add_header Content-Security-Policy "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; form-action 'self'" always;
        location = / {{ try_files /admin.html =404; }}
        location = /admin.js {{ try_files $uri =404; }}
        location = /admin.css {{ try_files $uri =404; }}
        location = /style.css {{ try_files $uri =404; }}
        location = /admin/api/v1/config {{
            limit_req zone=task5admin burst=5 nodelay;
            scgi_param SCGI 1;
            scgi_param REQUEST_METHOD $request_method;
            scgi_param PATH_INFO $uri;
            scgi_param QUERY_STRING $query_string;
            scgi_param HTTP_AUTHORIZATION $http_authorization;
            scgi_param HTTP_ORIGIN $http_origin;
            scgi_param CONTENT_TYPE $content_type;
            scgi_pass_request_headers off;
            scgi_pass_request_body on;
            scgi_read_timeout 3s;
            scgi_send_timeout 2s;
            scgi_buffering off;
            scgi_pass unix:{root}/run/config.sock;
        }}
        location / {{ return 404; }}
    }}
""".format(root=root,port=a.admin_port)
    rendered=rendered.rsplit('}',1)[0]+admin+'}\n'
(root/'nginx.conf').write_text(rendered,encoding='utf-8')
print(root/'nginx.conf')
