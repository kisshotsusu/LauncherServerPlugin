"""Run with: python -m unittest discover -s tests -v"""
import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from http.server import ThreadingHTTPServer
from urllib.request import Request, urlopen
from urllib.error import HTTPError
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_server as app
from config import load_config
from importer import import_version
from versions import build_versions_index

class PlatformHTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for key, root in [('a', self.root), ('b', self.root/'projects'/'b')]:
            root.mkdir(parents=True, exist_ok=True)
            bases = {}
            for platform in ['Windows', 'Linux']:
                base = root/'base'/platform
                base.mkdir(parents=True)
                (base/'game.txt').write_bytes((key+platform).encode())
                bases[platform] = {'1.0':str(base)}
                src=root/'source'/'1.1'/platform
                src.mkdir(parents=True)
                (src/(platform+'.pak')).write_bytes((key+platform+'patch').encode())
            (root/'source'/'1.1'/'1.1_Release.json').write_text(json.dumps({'baseVersionId':'1.0','date':'2026-10-08'}))
            disk={'project':key,'platforms':['Windows','Linux'],'default_platform':'Windows','data_dir':'data','version_library_dir':'library','base_packages':bases,'platform_settings':{p:{'patchSourceDir':'source','hotpatcherOrder':'1.1'} for p in ['Windows','Linux']}}
            (root/'config.json').write_text(json.dumps(disk))
            cfg=load_config(str(root/'config.json'))
            for platform in cfg['platforms']:
                import_version(str(root/'source'), cfg['versions_dir'], '1.1', platform, str(root/'base'/platform))
            build_versions_index(cfg)
        (self.root/'projects.json').write_text(json.dumps({'default':'a','projects':{'a':{'display':'A','config':'config.json'},'b':{'display':'B','config':'projects/b/config.json'}}}))
        app._PROJECT_CFG_CACHE.clear();app._CFG_MTIME_CACHE.clear()
        app.UpdateHandler.cfg=load_config(str(self.root/'config.json'))
        app.init_projects(str(self.root/'config.json'))
        class Quiet(app.UpdateHandler):
            def log_message(self,*args):pass
        self.http=ThreadingHTTPServer(('127.0.0.1',0),Quiet)
        self.thread=threading.Thread(target=self.http.serve_forever,daemon=True)
        self.thread.start()
        self.url='http://127.0.0.1:'+str(self.http.server_port)
    def tearDown(self):
        self.http.shutdown();self.http.server_close();self.thread.join();self.temp.cleanup()
    def request(self,path,body=None,method=None,headers=None,raw=False):
        request=Request(self.url+path,data=json.dumps(body).encode() if body is not None else None,method=method,headers={'Content-Type':'application/json',**(headers or {})})
        with urlopen(request,timeout=5) as response:
            data=response.read()
            return data if raw else json.loads(data)
    def error(self,path,code,**kwargs):
        with self.assertRaises(HTTPError) as caught:self.request(path,**kwargs)
        self.assertEqual(caught.exception.code,code)
    def test_all_platform_selectors(self):
        for path,headers in [('/a/Linux/api/versions',{}),('/a/platforms/Linux/api/versions',{}),('/a/api/versions?platform=linux',{}),('/a/api/versions',{'X-Platform':'Linux'})]:
            result=self.request(path,headers=headers)
            self.assertEqual(result['platform'],'Linux');self.assertEqual(len(result['versions']),2)
            self.assertEqual(result['updateChain'],['1.1'])
        self.assertEqual(self.request('/a/api/versions')['platform'],'Windows')
    def test_prefix_download_isolation(self):
        for key in ['a','b']:
            for platform in ['Windows','Linux']:
                desc=self.request(f'/{key}/{platform}/api/version/1.1')
                url=desc['files'][0]['url']
                self.assertEqual(url,f'/{key}/files/versions/{platform}/1.1/files/{platform}.pak')
                self.assertEqual(self.request(url,raw=True),(key+platform+'patch').encode())
                base=self.request(f'/{key}/{platform}/api/version/1.0')
                self.assertEqual(self.request(base['files'][0]['url'],raw=True),(key+platform).encode())
    def test_file_browser_is_not_mistaken_for_platform_route(self):
        result=self.request("/a/api/files?platform=Linux&baseVersion=1.0")
        self.assertEqual(result["platform"], "Linux")
        self.assertEqual(result["entries"][0]["name"], "game.txt")
    def test_unknown_and_conflicting_selectors(self):
        self.error('/a/Unknown/api/versions',400)
        self.error('/a/Linux/api/versions?platform=Windows',400)
        self.error('/a/api/versions?platform=Linux',400,headers={'X-Platform':'Windows'})
        self.error('/a/Linux/api/enabled_versions',400,body={'platform':'Windows','versions':[]})
    def test_enabled_versions_are_project_and_platform_scoped(self):
        self.request('/b/Linux/api/enabled_versions',{'versions':[]})
        self.assertEqual(self.request('/b/Linux/api/versions')['versions'],[])
        self.assertEqual(len(self.request('/b/Windows/api/versions')['versions']),2)
        self.assertEqual(len(self.request('/a/Linux/api/versions')['versions']),2)
        self.assertEqual(self.request('/b/Linux/api/versions')['revoked'][0]['platform'],'Linux')
    def test_delete_only_current_platform(self):
        self.request('/a/Windows/api/version/1.1',method='DELETE')
        self.error('/a/Windows/api/version/1.1',404)
        self.assertEqual(self.request('/a/Linux/api/version/1.1')['platform'],'Linux')
        self.assertEqual(self.request('/b/Windows/api/version/1.1')['platform'],'Windows')
    def test_config_write_and_relative_paths_stay_in_project(self):
        self.request('/b/Linux/api/platform/config',{'basePackagesRoot':'base','basePackages':{'2.0':'Linux'},'patchSourceDir':'source','hotpatcherOrder':'1.1'})
        config=self.request('/b/Linux/api/platform/config')['config']
        self.assertEqual(config['basePackages']['2.0'],str(self.root/'projects'/'b'/'base'/'Linux'))
        self.assertEqual(config['patchSourceDir'],str(self.root/'projects'/'b'/'source'))
        self.assertEqual(list(self.request('/b/Windows/api/platform/config')['config']['basePackages']),['1.0'])
        self.assertEqual(list(self.request('/a/Linux/api/platform/config')['config']['basePackages']),['1.0'])
    def test_import_uses_platform_layout_and_project_library(self):
        self.request('/b/Linux/api/import/hotpatcher',{})
        self.assertTrue((self.root/'projects'/'b'/'library'/'Linux'/'1.1'/'descriptor.json').exists())
        self.assertFalse((self.root/'projects'/'b'/'data'/'versions').exists())
    def test_reimport_removes_obsolete_platform_files(self):
        cfg=load_config(str(self.root/'config.json'))
        old=Path(cfg['versions_dir'])/'Linux'/'1.1'/'files'/'obsolete.pak'
        old.write_bytes(b'old')
        self.request('/a/Linux/api/import/hotpatcher',{})
        self.assertFalse(old.exists())
        self.assertTrue((Path(cfg['versions_dir'])/'Windows'/'1.1'/'files'/'Windows.pak').exists())
    def test_failed_import_keeps_published_version(self):
        cfg=load_config(str(self.root/'config.json'))
        with self.assertRaises(ValueError):
            import_version(str(self.root/'missing-source'),cfg['versions_dir'],'1.1','Linux','')
        self.assertTrue((Path(cfg['versions_dir'])/'Linux'/'1.1'/'descriptor.json').exists())
    def test_project_platform_manager(self):
        result=self.request('/a/api/projects/platforms',{'key':'b','platforms':['Windows','Linux','Android'],'defaultPlatform':'Linux'})
        self.assertTrue(result['ok']);self.assertEqual(self.request('/b/api/versions')['platform'],'Linux')
        listing=self.request('/a/api/projects')['projects']
        self.assertEqual(next(p for p in listing if p['key']=='b')['platforms'],['Windows','Linux','Android'])
        self.error('/a/api/projects/platforms',400,body={'key':'b','platforms':['Windows','windows']})
    def test_create_project_with_selected_platforms(self):
        self.request('/a/api/projects',{'key':'c','display':'C','copyFrom':'a','platforms':['Linux','Android']})
        self.assertEqual(self.request('/c/api/config')['config']['platforms'],['Linux','Android'])
    def test_malformed_body_and_deprecated_file_route(self):
        self.error('/a/api/platform/config',400,body=[])
        self.error('/a/files/versions/1.1/Windows/Windows.pak',400)
        self.error('/a/Linux/api/platform/config',400,body={'basePackages':{'../bad':'base'}})

    def test_workspace_initialization_and_base_import(self):
        workspace=self.root/'workspace'
        self.request('/a/api/workspace',{'root':str(workspace)})
        view=self.request('/a/api/workspace')
        self.assertTrue(view['configured'])
        self.assertEqual(view['paths']['data'],str(workspace/'data'))
        for platform in ['Windows','Linux']:
            self.assertTrue((workspace/'imports'/platform/'base').is_dir())
            self.assertTrue((workspace/'patches'/platform).is_dir())
        source=workspace/'imports'/'Linux'/'base'/'2.0';source.mkdir()
        (source/'game.txt').write_bytes(b'linux-base')
        self.request('/a/api/workspace/import',{'category':'base','platform':'Linux','version':'2.0'})
        desc=self.request('/a/Linux/api/version/2.0')
        self.assertEqual(self.request(desc['files'][0]['url'],raw=True),b'linux-base')
        self.assertEqual(self.request('/a/Windows/api/versions')['versions'],[])
        self.error('/a/api/workspace/import',409,body={'category':'base','platform':'Linux','version':'2.0'})
    def test_workspace_patch_and_launcher_import(self):
        workspace=self.root/'workspace'
        self.request('/a/api/workspace',{'root':str(workspace)})
        source=workspace/'imports'/'Linux'/'patch'/'2.1';(source/'Linux').mkdir(parents=True)
        (source/'2.1_Release.json').write_text(json.dumps({'baseVersionId':'2.0'}))
        (source/'Linux'/'game.pak').write_bytes(b'patch')
        self.request('/a/api/workspace/import',{'category':'patch','platform':'Linux'})
        self.assertTrue((workspace/'patches'/'Linux'/'2.1'/'descriptor.json').exists())
        launcher=workspace/'imports'/'launcher'/'3.0';launcher.mkdir()
        (launcher/'Launcher.exe').write_bytes(b'test-launcher')
        self.request('/a/api/workspace/import',{'category':'launcher','version':'3.0'})
        self.assertTrue((workspace/'launcher'/'versions'/'3.0'/'Launcher.exe').exists())
        self.assertEqual(self.request('/a/api/launcher/versions')['versions'][0]['version'],'3.0')
    def test_workspace_move_preserves_data_and_rejects_nested_target(self):
        workspace=self.root/'workspace';target=self.root/'new-space'
        self.request('/a/api/workspace',{'root':str(workspace)})
        (workspace/'data'/'marker.txt').write_text('keep')
        self.error('/a/api/workspace',400,body={'root':str(workspace/'nested')})
        self.request('/a/api/workspace',{'root':str(target)})
        self.assertFalse(workspace.exists())
        self.assertEqual((target/'data'/'marker.txt').read_text(),'keep')
        self.assertEqual(self.request('/a/api/workspace')['paths']['root'],str(target))

if __name__=='__main__':unittest.main()
