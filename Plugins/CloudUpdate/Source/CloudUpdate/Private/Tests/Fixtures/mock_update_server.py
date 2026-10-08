from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from urllib.parse import urlparse
from pathlib import Path
import json,time,sys
payloads={'first.bin':b'A'*16384,'second.bin':b'B'*32768,'base.bin.patch':b'C'*32,'fallback.bin.patch':b'FAIL1234','fallback.bin':b'D'*128}
def entry(name,**kw):return {'fileName':name,'targetRelativePath':name,'url':'/files/'+name,'size':len(payloads[name]),'kind':'ExternFile',**kw}
descs={
 '2.0':{'versionId':'2.0','type':'full','files':[{'fileName':'game.exe','targetRelativePath':'game.exe','url':'/game/game.exe','size':10,'kind':'ExternFile'},{'fileName':'game.dll','targetRelativePath':'game.dll','url':'/game/game.dll','size':20,'kind':'ExternFile'}]},
 '1.1':{'versionId':'1.1','resourceVersion':'1.1','requiredGameVersion':'1.0','type':'patch','files':[entry('first.bin'),entry('base.bin.patch',binaryPatch=True)]},
 '1.2':{'versionId':'1.2','resourceVersion':'1.2','requiredGameVersion':'1.0','type':'patch','files':[entry('second.bin'),entry('fallback.bin.patch',binaryPatch=True,fallbackUrl='/files/fallback.bin',fallbackSize=128)]},
 '1.3':{'versionId':'1.3','resourceVersion':'1.3','requiredGameVersion':'2.0','type':'patch','files':[entry('first.bin')]}}
index={'current':'2.0','gameVersion':'2.0','resourceVersion':'1.3','updateChain':['1.1','1.2','1.3'],'baseVersions':{'Windows':['2.0']},'versions':[{'versionId':k,'resourceVersion':v.get('resourceVersion',k),'type':v['type'],'requiredGameVersion':v.get('requiredGameVersion','')} for k,v in descs.items()]}
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  path=urlparse(self.path).path
  if path=='/api/versions': body=json.dumps(index).encode();kind='application/json'
  elif path.startswith('/api/version/'):
   desc=descs.get(path.rsplit('/',1)[-1])
   if not desc:self.send_error(404);return
   body=json.dumps(desc).encode();kind='application/json'
  elif path.startswith('/files/') and path[7:] in payloads:body=payloads[path[7:]];kind='application/octet-stream'
  else:self.send_error(404);return
  self.send_response(200);self.send_header('Content-Type',kind);self.send_header('Content-Length',str(len(body)));self.end_headers()
  for offset in range(0,len(body),2048):
   self.wfile.write(body[offset:offset+2048]);self.wfile.flush()
   if kind!='application/json':time.sleep(.02)
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
Path(sys.argv[1]).write_text(str(server.server_port),encoding='utf-8')
server.serve_forever()
