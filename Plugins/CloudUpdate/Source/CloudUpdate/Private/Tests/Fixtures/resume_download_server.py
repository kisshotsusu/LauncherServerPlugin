from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse
import json, socket, sys, time
DATA = bytes(range(256)) * 4096
counts = {}; events = []
class Handler(BaseHTTPRequestHandler):
 def log_message(self, *args): pass
 def do_GET(self):
  path=urlparse(self.path).path
  if path=='/stats':
   body=json.dumps(events).encode();self.send_response(200);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body);return
  counts[path]=counts.get(path,0)+1; attempt=counts[path]
  offset=int(self.headers.get('Range','bytes=0-')[6:].split('-')[0])
  event={'path':path,'attempt':attempt,'offset':offset,'ifRange':self.headers.get('If-Range')};events.append(event)
  if path.endswith('/empty'):
   self.send_response(200);self.send_header('Content-Length','0');self.end_headers();event.update(status=200,sent=0);return
  status=206 if offset else 200
  if path.endswith('/error') and attempt==2:
   self.send_response(503);self.send_header('Content-Length','5');self.end_headers();self.wfile.write(b'error');event.update(status=503,sent=5);return
  if path.endswith('/ignore') or (path.endswith('/change') and attempt>1):status=200;offset=0
  self.send_response(status)
  self.send_header('Content-Length',str(len(DATA)-offset))
  self.send_header('ETag','"new"' if path.endswith('/change') and attempt>1 else '"v1"')
  if status==206:
   start=offset+1 if path.endswith('/bad-range') and attempt==2 else offset
   self.send_header('Content-Range',f'bytes {start}-{len(DATA)-1}/{len(DATA)}')
  self.end_headers()
  body=DATA[offset:]
  if attempt==1:body=body[:131072]
  sent=0
  try:
   for i in range(0,len(body),16384):
    part=body[i:i+16384];self.wfile.write(part);self.wfile.flush();sent+=len(part);time.sleep(.025)
  except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError):pass
  event.update(status=status,sent=sent)
  if attempt==1:
   self.close_connection=True
   try:self.connection.shutdown(socket.SHUT_RDWR)
   except OSError:pass
server=ThreadingHTTPServer(('127.0.0.1',int(sys.argv[2]) if len(sys.argv)>2 else 0),Handler)
Path(sys.argv[1]).write_text(str(server.server_port))
server.serve_forever()
