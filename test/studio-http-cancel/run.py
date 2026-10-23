#!/usr/bin/env python3
"""Loopback only, synthetic requests. Does not contact OAuth/Drive or credentials."""
import collections, http.server, json, pathlib, subprocess, sys, threading, time
binary, output = sys.argv[1:]
counts = collections.Counter()
lock = threading.Lock()
class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *_): pass
    def request(self):
        with lock:
            counts[self.path] += 1
            number = counts[self.path]
        if self.path.startswith('/upload-stall'): time.sleep(2)
        length = int(self.headers.get('Content-Length', '0'))
        if length: self.rfile.read(length)
        status = 503 if self.path.startswith('/retry') and number < 3 else 200
        try:
            if self.path.startswith('/hold'): time.sleep(2)
            if self.path.startswith('/stream'):
                self.send_response(status); self.send_header('Content-Length', '4000'); self.end_headers()
                for _ in range(40):
                    self.wfile.write(b'x'*100); self.wfile.flush(); time.sleep(.05)
            else:
                self.send_response(status); self.send_header('Content-Length', '2'); self.end_headers()
                self.wfile.write(b'{}'); self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError): pass
    do_GET = do_POST = do_PUT = do_DELETE = request
server = Server(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
base = 'http://127.0.0.1:' + str(server.server_port)
checks = []
results = []
def check(value, label): checks.append({'name': label, 'passed': bool(value)})
def run(name, method, path, mode, delay, size=0):
    completed = subprocess.run([binary, method, base+path, mode, str(delay), str(size)], capture_output=True, text=True, timeout=12)
    value = json.loads(completed.stdout)
    with lock: n = counts[path]
    results.append({'name': name, 'method': method, 'mode': mode, 'result': value, 'requests': n, 'exit': completed.returncode})
    check(completed.returncode == 0, name+' helper completes')
    return value, n
try:
    for method in ('GET','JSON','PUT','DELETE'):
        name = 'pre-cancel '+method
        r,n=run(name, method, '/pre-'+method, 'operation', 0)
        check(not r['ok'] and r['cancelled_error'] and n == 0, name+' no request or success')
    for method in ('GET','JSON','PUT','DELETE'):
        name='active-cancel '+method
        r,n=run(name, method, '/stream-'+method, 'operation', 120, 4096 if method=='PUT' else 0)
        check(not r['ok'] and r['cancelled_error'] and n == 1, name+' unconfirmed without retry')
        check(50 <= r['duration_ms'] < 1200, name+' local progress cancellation latency')
    r,n=run('upload-stall', 'PUT', '/upload-stall', 'operation', 120, 8*1024*1024)
    check(not r['ok'] and r['cancelled_error'] and n==1, 'outbound upload aborts without retry')
    check(r['duration_ms'] < 1800, 'local outbound progress cancellation latency')
    r,n=run('shutdown-idle', 'GET', '/hold-shutdown', 'shutdown', 120)
    check(not r['ok'] and r['cancelled_error'] and n==1, 'shutdown token aborts idle response')
    check(r['duration_ms'] < 1800, 'local idle progress cancellation latency')
    for method in ('GET','JSON','PUT'):
        name='backoff-cancel '+method
        r,n=run(name, method, '/retry-cancel-'+method, 'shutdown', 120)
        check(not r['ok'] and r['cancelled_error'] and n==1, name+' no next attempt')
        check(r['duration_ms'] < 800, name+' interrupts retry sleep')
    r,n=run('default-retry', 'GET', '/retry-default', 'none', -1)
    check(r['ok'] and r['status']==200 and n==3, 'default caller retains three attempts')
    check(r['duration_ms'] >= 2800, 'default backoff unchanged')
    r,n=run('enabled-not-cancelled', 'JSON', '/normal-json', 'operation', -1)
    check(r['ok'] and r['status']==200 and n==1, 'opt-in noncancelled request succeeds')
    r,n=run('form-no-cancel', 'FORM', '/form', 'operation', 0)
    check(r['ok'] and r['status']==200 and n==1, 'PostForm remains outside cancellation scope')
finally:
    server.shutdown(); server.server_close(); thread.join()
receipt={'date':'2026-10-23','scope':'real libcurl; controlled loopback HTTP only; synthetic data; no OAuth refresh or public endpoint',
         'checks':len(checks),'failures':sum(not c['passed'] for c in checks),'passed':all(c['passed'] for c in checks),'cases':results,'assertions':checks}
pathlib.Path(output).write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({k:receipt[k] for k in ('checks','failures','passed')}))
sys.exit(0 if receipt['passed'] else 1)
