"""Run with: python -m unittest discover -s tests -v"""
import json
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
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
            for platform in ['Windows', 'IOS']:
                base = root/'base'/platform
                base.mkdir(parents=True)
                (base/'game.txt').write_bytes((key+platform).encode())
                (base/'zz_base.pak').write_bytes(b'pak')
                bases[platform] = {'1.0':str(base)}
                src=root/'source'/'1.1'/platform
                src.mkdir(parents=True)
                (src/(platform+'.pak')).write_bytes((key+platform+'patch').encode())
            (root/'source'/'1.1'/'1.1_Release.json').write_text(json.dumps({'baseVersionId':'1.0','date':'2026-10-08'}))
            disk={'project':key,'platforms':['Windows','IOS'],'default_platform':'Windows','data_dir':'data','version_library_dir':'library','base_packages':bases,'platform_settings':{p:{'patchSourceDir':'source','hotpatcherOrder':'1.1'} for p in ['Windows','IOS']}}
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
    def test_missing_project_config_returns_json(self):
        (self.root/'projects'/'b'/'config.json').unlink()
        with self.assertRaises(HTTPError) as caught:
            self.request('/b/api/projects')
        self.assertEqual(caught.exception.code, 404)
        result = json.load(caught.exception)
        self.assertFalse(result['ok'])
        self.assertEqual(result['project'], 'b')
        self.assertIn('配置文件缺失', result['error'])

    def test_all_platform_selectors(self):
        for path,headers in [('/a/IOS/api/versions',{}),('/a/platforms/IOS/api/versions',{}),('/a/api/versions?platform=ios',{}),('/a/api/versions',{'X-Platform':'IOS'})]:
            result=self.request(path,headers=headers)
            self.assertEqual(result['platform'],'IOS');self.assertEqual(len(result['versions']),2)
            self.assertEqual(result['updateChain'],['1.1'])
        self.assertEqual(self.request('/a/api/versions')['platform'],'Windows')
    def test_prefix_download_isolation(self):
        for key in ['a','b']:
            for platform in ['Windows','IOS']:
                desc=self.request(f'/{key}/{platform}/api/version/1.1')
                url=desc['files'][0]['url']
                self.assertEqual(url,f'/{key}/files/versions/{platform}/1.1/files/{platform}.pak')
                self.assertEqual(self.request(url,raw=True),(key+platform+'patch').encode())
                base=self.request(f'/{key}/{platform}/api/version/1.0')
                self.assertEqual(self.request(base['files'][0]['url'],raw=True),(key+platform).encode())
    def test_file_browser_is_not_mistaken_for_platform_route(self):
        result=self.request("/a/api/files?platform=IOS&baseVersion=1.0")
        self.assertEqual(result["platform"], "IOS")
        self.assertEqual(result["entries"][0]["name"], "game.txt")
    def test_unknown_and_conflicting_selectors(self):
        self.error('/a/Unknown/api/versions',400)
        self.error('/a/IOS/api/versions?platform=Windows',400)
        self.error('/a/api/versions?platform=IOS',400,headers={'X-Platform':'Windows'})
        self.error('/a/IOS/api/enabled_versions',400,body={'platform':'Windows','versions':[]})
    def test_enabled_versions_are_project_and_platform_scoped(self):
        self.request('/b/IOS/api/enabled_versions',{'versions':[]})
        self.assertEqual(self.request('/b/IOS/api/versions')['versions'],[])
        self.assertEqual(len(self.request('/b/Windows/api/versions')['versions']),2)
        self.assertEqual(len(self.request('/a/IOS/api/versions')['versions']),2)
        self.assertEqual(self.request('/b/IOS/api/versions')['revoked'][0]['platform'],'IOS')
    def test_delete_only_current_platform(self):
        self.request('/a/Windows/api/version/1.1',method='DELETE')
        self.error('/a/Windows/api/version/1.1',404)
        self.assertEqual(self.request('/a/IOS/api/version/1.1')['platform'],'IOS')
        self.assertEqual(self.request('/b/Windows/api/version/1.1')['platform'],'Windows')
    def test_config_write_and_relative_paths_stay_in_project(self):
        self.request('/b/IOS/api/platform/config',{'basePackagesRoot':'base','basePackages':{'2.0':'IOS'},'patchSourceDir':'source','hotpatcherOrder':'1.1'})
        config=self.request('/b/IOS/api/platform/config')['config']
        self.assertEqual(config['basePackages']['2.0'],str(self.root/'projects'/'b'/'base'/'IOS'))
        self.assertEqual(config['patchSourceDir'],str(self.root/'projects'/'b'/'source'))
        self.assertEqual(list(self.request('/b/Windows/api/platform/config')['config']['basePackages']),['1.0'])
        self.assertEqual(list(self.request('/a/IOS/api/platform/config')['config']['basePackages']),['1.0'])
    def test_import_uses_platform_layout_and_project_library(self):
        self.request('/b/IOS/api/import/hotpatcher',{})
        self.assertTrue((self.root/'projects'/'b'/'library'/'IOS'/'1.1'/'descriptor.json').exists())
        self.assertFalse((self.root/'projects'/'b'/'data'/'versions').exists())
    def test_reimport_removes_obsolete_platform_files(self):
        cfg=load_config(str(self.root/'config.json'))
        old=Path(cfg['versions_dir'])/'IOS'/'1.1'/'files'/'obsolete.pak'
        old.write_bytes(b'old')
        self.request('/a/IOS/api/import/hotpatcher',{})
        self.assertFalse(old.exists())
        self.assertTrue((Path(cfg['versions_dir'])/'Windows'/'1.1'/'files'/'Windows.pak').exists())
    def test_failed_import_keeps_published_version(self):
        cfg=load_config(str(self.root/'config.json'))
        with self.assertRaises(ValueError):
            import_version(str(self.root/'missing-source'),cfg['versions_dir'],'1.1','IOS','')
        self.assertTrue((Path(cfg['versions_dir'])/'IOS'/'1.1'/'descriptor.json').exists())
    def test_project_platform_manager(self):
        result=self.request('/a/api/projects/platforms',{'key':'b','platforms':['Windows','IOS','Android'],'defaultPlatform':'IOS'})
        self.assertTrue(result['ok']);self.assertEqual(self.request('/b/api/versions')['platform'],'IOS')
        listing=self.request('/a/api/projects')['projects']
        self.assertEqual(next(p for p in listing if p['key']=='b')['platforms'],['Windows','IOS','Android'])
        self.error('/a/api/projects/platforms',400,body={'key':'b','platforms':['Windows','windows']})
    def test_create_project_with_selected_platforms(self):
        self.request('/a/api/projects',{'key':'c','display':'C','copyFrom':'a','platforms':['IOS','Android']})
        self.assertEqual(self.request('/c/api/config')['config']['platforms'],['IOS','Android'])
    def test_malformed_body_and_deprecated_file_route(self):
        self.error('/a/api/platform/config',400,body=[])
        self.error('/a/files/versions/1.1/Windows/Windows.pak',400)
        self.error('/a/IOS/api/platform/config',400,body={'basePackages':{'../bad':'base'}})

    def test_workspace_initialization_and_base_import(self):
        workspace=self.root/'workspace'/'a'
        self.request('/a/api/workspace',{'root':str(workspace)})
        view=self.request('/a/api/workspace')
        self.assertTrue(view['configured'])
        self.assertEqual(view['paths']['data'],str(workspace/'data'))
        for platform in ['Windows','IOS']:
            self.assertTrue((workspace/'imports'/platform/'base').is_dir())
            self.assertTrue((workspace/'patches'/platform).is_dir())
        source=workspace/'imports'/'IOS'/'base'/'2.0';source.mkdir()
        (source/'game.txt').write_bytes(b'ios-base');(source/'zz_base.pak').write_bytes(b'pak')
        self.request('/a/api/workspace/import',{'category':'base','platform':'IOS','version':'2.0'})
        desc=self.request('/a/IOS/api/version/2.0')
        self.assertEqual(self.request(desc['files'][0]['url'],raw=True),b'ios-base')
        self.assertEqual(self.request('/a/Windows/api/versions')['versions'],[])
        self.error('/a/api/workspace/import',409,body={'category':'base','platform':'IOS','version':'2.0'})
    def test_workspace_patch_and_launcher_import(self):
        workspace=self.root/'workspace'/'a'
        self.request('/a/api/workspace',{'root':str(workspace)})
        source=workspace/'imports'/'IOS'/'patch'/'2.1';(source/'IOS').mkdir(parents=True)
        (source/'2.1_Release.json').write_text(json.dumps({'baseVersionId':'2.0'}))
        (source/'IOS'/'game.pak').write_bytes(b'patch')
        self.request('/a/api/workspace/import',{'category':'patch','platform':'IOS'})
        self.assertTrue((workspace/'patches'/'IOS'/'2.1'/'descriptor.json').exists())
        launcher=workspace/'imports'/'launcher'/'3.0';launcher.mkdir()
        (launcher/'Launcher.exe').write_bytes(b'test-launcher')
        self.request('/a/api/workspace/import',{'category':'launcher','version':'3.0'})
        self.assertTrue((workspace/'launcher'/'versions'/'3.0'/'Launcher.exe').exists())
        self.assertEqual(self.request('/a/api/launcher/versions')['versions'][0]['version'],'3.0')
    def test_workspace_move_preserves_data_and_rejects_nested_target(self):
        workspace=self.root/'workspace'/'a';target=self.root/'new-space'
        self.request('/a/api/workspace',{'root':str(workspace)})
        (workspace/'data'/'marker.txt').write_text('keep')
        self.error('/a/api/workspace',400,body={'root':str(workspace/'nested')})
        self.request('/a/api/workspace',{'root':str(target)})
        self.assertFalse(workspace.exists())
        self.assertEqual((target/'data'/'marker.txt').read_text(),'keep')
        self.assertEqual(self.request('/a/api/workspace')['paths']['root'],str(target))

    def test_pending_imports_exclude_empty_invalid_and_imported_versions(self):
        workspace=self.root/'workspace'/'a'
        self.request('/a/api/workspace',{'root':str(workspace)})
        inbox=workspace/'imports'/'IOS'/'base'
        for version in ['2.0','empty','bad version']:
            (inbox/version).mkdir()
        (inbox/'2.0'/'game.txt').write_bytes(b'base');(inbox/'2.0'/'zz_base.pak').write_bytes(b'pak')
        (inbox/'bad version'/'game.txt').write_bytes(b'base')
        view=self.request('/a/api/workspace')
        self.assertEqual(view['platforms'][1]['pendingBases'],['2.0'])
        self.request('/a/api/workspace/import',{'category':'base','platform':'IOS','version':'2.0'})
        self.assertEqual(self.request('/a/api/workspace')['platforms'][1]['pendingBases'],[])
        launcher=workspace/'imports'/'launcher'/'3.0';launcher.mkdir()
        (launcher/'Launcher.exe').write_bytes(b'launcher')
        self.assertEqual(self.request('/a/api/workspace')['pendingLaunchers'],['3.0'])
        self.request('/a/api/workspace/import',{'category':'launcher','version':'3.0'})
        self.assertEqual(self.request('/a/api/workspace')['pendingLaunchers'],[])
        self.request('/a/api/launcher/versions/3.0',method='DELETE')
        self.assertFalse((workspace/'launcher'/'versions'/'3.0').exists())
        self.assertTrue(list((workspace/'data'/'trash'/'launcher').glob('3.0-*')))

    def test_workspace_rejects_files_and_cross_project_overlap(self):
        workspace=self.root/'workspace'/'a'
        self.request('/a/api/workspace',{'root':str(workspace)})
        file=self.root/'target.txt';file.write_text('keep')
        self.error('/a/api/workspace',400,body={'root':str(file)})
        for target in [workspace,workspace/'child',self.root]:
            self.error('/b/api/workspace',400,body={'root':str(target)})
        self.error('/a/api/platform/config',400,body={'patchSourceDir':'external'})
        self.assertEqual(file.read_text(),'keep')
        self.assertEqual(self.request('/a/api/workspace')['paths']['root'],str(workspace))

    def test_workspace_failed_config_save_restores_directory(self):
        workspace=self.root/'workspace'/'a';target=self.root/'moved'
        self.request('/a/api/workspace',{'root':str(workspace)})
        (workspace/'data'/'marker').write_text('keep')
        with patch.object(app,'_apply_config_updates',side_effect=OSError('write failed')):
            self.error('/a/api/workspace',400,body={'root':str(target)})
        self.assertEqual((workspace/'data'/'marker').read_text(),'keep')
        self.assertFalse(target.exists())

    def test_workspace_move_updates_internal_certificate_paths(self):
        workspace=self.root/'workspace'/'a';target=self.root/'moved'
        self.request('/a/api/workspace',{'root':str(workspace)})
        disk=json.loads((self.root/'config.json').read_text())
        disk['https']={'enabled':False,'certFile':str(workspace/'data'/'ssl'/'server.crt'),'keyFile':str(workspace/'data'/'ssl'/'server.key')}
        (self.root/'config.json').write_text(json.dumps(disk))
        self.request('/a/api/workspace',{'root':str(target)})
        saved=json.loads((self.root/'config.json').read_text())
        self.assertEqual(saved['https']['certFile'],str(target/'data'/'ssl'/'server.crt'))

    def test_nested_patch_files_and_project_asset_prefixes(self):
        import importer
        cfg=load_config(str(self.root/'config.json'))
        cfg['game_asset_prefixes']=['/Custom/']
        source=self.root/'source'/'1.1'
        nested=source/'IOS'/'nested';nested.mkdir()
        (nested/'nested.pak').write_bytes(b'nested-patch')
        diff={'assetDiffInfo':{'addAssetDependInfo':{'assetsDependenciesMap':{'Custom':{'assetDependencyDetails':{'/Custom/Item':{},'/Game/Other':{}}}}}}}
        (source/'1.1_Diff.json').write_text(json.dumps(diff))
        with patch.object(importer,'load_config',side_effect=AssertionError('global config must not be read')):
            desc,_=import_version(str(self.root/'source'),cfg['versions_dir'],'1.1','IOS','',config=cfg)
        self.assertEqual(desc['changedAssetCount'],1)
        nested_file=next(f for f in desc['files'] if f['targetRelativePath']=='nested/nested.pak')
        self.assertEqual(nested_file['fileName'],'nested.pak')
        self.assertEqual(self.request('/a'+nested_file['url'],raw=True),b'nested-patch')

    def test_android_data_and_patch_imports_have_platform_scoped_downloads(self):
        self.request('/a/api/projects/platforms', {'key': 'a', 'platforms': ['Windows', 'IOS', 'Android']})
        self.request('/a/api/workspace', {'root': str(self.root / 'workspace' / 'a')})
        release = self.root / 'android-release'
        release.mkdir()
        (release / 'Game.apk').write_bytes(b'apk')
        self.error('/a/api/workspace/import', 400, body={'category': 'base', 'platform': 'Android', 'version': '1.0', 'source': str(release)})
        pak = release / 'Data' / 'Game' / 'Content' / 'Paks' / 'base.pak'
        pak.parent.mkdir(parents=True)
        pak.write_bytes(b'android-base')
        import zipfile
        with zipfile.ZipFile(release / 'Game.apk', 'w') as zf: zf.writestr('AndroidManifest.xml', b'm')
        self.request('/a/api/workspace/import', {'category': 'base', 'platform': 'Android', 'version': '1.0', 'source': str(release)})
        base = self.request('/a/Android/api/version/1.0')
        self.assertEqual(base['files'][0]['targetRelativePath'], 'Game/Content/Paks/base.pak')
        self.assertEqual(self.request(base['files'][0]['url'], raw=True), b'android-base')
        source = self.root / 'mixed-patches'
        for version, platform in [('1.1', 'Android'), ('1.1', 'Windows'), ('1.2', 'Windows')]:
            folder = source / version
            (folder / platform).mkdir(parents=True, exist_ok=True)
            (folder / (version + '_Release.json')).write_text(json.dumps({'baseVersionId': '1.0'}))
            (folder / platform / 'game_P.pak').write_bytes(platform.encode())
        self.request('/a/api/workspace/import', {'category': 'patch', 'platform': 'Android', 'source': str(source)})
        desc = self.request('/a/Android/api/version/1.1')
        self.assertEqual(self.request(desc['files'][0]['url'], raw=True), b'Android')
        self.assertEqual(self.request('/a/Android/api/versions')['updateChain'], ['1.1'])
        self.assertEqual(self.request('/a/Windows/api/versions')['versions'], [])
        self.error('/a/Android_ASTC/api/version/1.1', 400)

    def test_android_apk_install_package_import_and_download(self):
        import zipfile
        self.request('/a/api/projects/platforms', {'key': 'a', 'platforms': ['Windows', 'Android']})
        self.request('/a/api/workspace', {'root': str(self.root / 'workspace' / 'a')})
        apk_dir = self.root / 'apk-src'
        apk_dir.mkdir()
        apk_path = apk_dir / 'Game-arm64.apk'
        with zipfile.ZipFile(apk_path, 'w') as zf:
            zf.writestr('AndroidManifest.xml', b'm')
            zf.writestr('classes.dex', b'd')
        # 非法 APK 拒绝
        bad = self.root / 'bad.apk'
        bad.write_bytes(b'nope')
        self.error('/a/api/workspace/import', 400, body={'category': 'apk', 'platform': 'Android', 'version': '1.0', 'source': str(bad)})
        # 合法 APK 导入
        self.request('/a/api/workspace/import', {'category': 'apk', 'platform': 'Android', 'version': '1.0', 'source': str(apk_dir)})
        listing = self.request('/a/Android/api/installers')
        self.assertEqual(len(listing['installers']), 1)
        entry = listing['installers'][0]
        self.assertEqual(entry['versionId'], '1.0')
        self.assertTrue(entry['fileName'].endswith('.apk'))
        body = self.request(entry['url'], raw=True)
        self.assertEqual(body[:2], b'PK')
        # 详情接口
        detail = self.request('/a/Android/api/installer/1.0')
        self.assertEqual(detail['installer']['versionId'], '1.0')
        # 基础包带 APK 时自动提取安装包
        release = self.root / 'mobile-release'
        pak = release / 'Data' / 'Game' / 'Content' / 'Paks' / 'game.pak'
        pak.parent.mkdir(parents=True)
        pak.write_bytes(b'pak-data')
        with zipfile.ZipFile(release / 'BiNanSuo-arm64.apk', 'w') as zf:
            zf.writestr('AndroidManifest.xml', b'm')
        (release / 'DeviceLogs').mkdir()
        (release / 'DeviceLogs' / 'x.log').write_text('junk')
        self.request('/a/api/workspace/import', {'category': 'base', 'platform': 'Android', 'version': '2.0', 'source': str(release)})
        # 资源包不含垃圾
        files = self.request('/a/Android/api/files?baseVersion=2.0')
        paths = [f.get('path', f.get('name', '')) for f in files.get('entries', files.get('files', []))]
        joined = json.dumps(files)
        self.assertNotIn('DeviceLogs', joined)
        self.assertNotIn('.apk', joined.lower().replace('installer', ''))
        # 自动提取的安装包
        listing = self.request('/a/Android/api/installers')
        ids = [i['versionId'] for i in listing['installers']]
        self.assertIn('2.0', ids)

    def test_delta_import_serves_selected_platform_patch_and_fallback(self):
        source = self.root / 'delta-source'
        platform_folder = source / '1.2' / 'IOS'
        platform_folder.mkdir(parents=True)
        (source / '1.2' / '1.2_Release.json').write_text(json.dumps({'baseVersionId': '1.0'}))
        (platform_folder / 'game.pak').write_bytes(b'full')
        (platform_folder / 'game.pak.patch').write_bytes(b'delta')
        self.request('/a/api/import/hotpatcher', {'platform': 'IOS', 'source': str(source)})
        desc = self.request('/a/IOS/api/version/1.2')
        self.assertEqual(len(desc['files']), 1)
        entry = desc['files'][0]
        self.assertEqual(self.request(entry['url'], raw=True), b'delta')
        self.assertEqual(self.request(entry['fallbackUrl'], raw=True), b'full')

    def test_manual_base_source_is_copied_and_remembered_per_platform(self):
        workspace=self.root/'workspace'/'a'
        self.request('/a/api/workspace',{'root':str(workspace)})
        source=self.root/'external-base';source.mkdir()
        (source/'game.txt').write_bytes(b'original');(source/'zz_base.pak').write_bytes(b'pak')
        self.request('/a/api/workspace/import',{'category':'base','platform':'IOS','version':'3.0','source':str(source)})
        (source/'game.txt').write_bytes(b'changed')
        desc=self.request('/a/IOS/api/version/3.0')
        self.assertEqual(self.request(desc['files'][0]['url'],raw=True),b'original')
        view=self.request('/a/api/workspace')
        self.assertEqual(view['platforms'][1]['sources']['base'],str(source))
        self.assertEqual(view['platforms'][0]['sources'],{})
        self.assertTrue(source.exists())
        self.error('/a/api/workspace/import',409,body={'category':'base','platform':'IOS','version':'3.0','source':str(source)})

    def test_manual_patch_source_accepts_single_version_and_root(self):
        workspace=self.root/'workspace'/'a'
        self.request('/a/api/workspace',{'root':str(workspace)})
        source=self.root/'external-patches'
        for version in ['3.1','3.2']:
            folder=source/version; (folder/'IOS').mkdir(parents=True)
            (folder/(version+'_Release.json')).write_text(json.dumps({'baseVersionId':'3.0'}))
            (folder/'IOS'/'game.pak').write_bytes(version.encode())
        self.request('/a/api/workspace/import',{'category':'patch','platform':'IOS','source':str(source/'3.1')})
        self.assertFalse((workspace/'patches'/'IOS'/'3.2').exists())
        self.request('/a/api/workspace/import',{'category':'patch','platform':'IOS','source':str(source)})
        desc=self.request('/a/IOS/api/version/3.2')
        self.assertEqual(self.request(desc['files'][0]['url'],raw=True),b'3.2')
        self.assertTrue((source/'3.2'/'IOS'/'game.pak').exists())
        self.assertEqual(self.request('/a/api/workspace')['platforms'][1]['sources']['patch'],str(source))
        self.assertEqual(self.request('/a/Windows/api/versions')['versions'],[])

    def test_manual_import_rejects_missing_empty_and_overlapping_sources(self):
        workspace=self.root/'workspace'/'a'
        self.request('/a/api/workspace',{'root':str(workspace)})
        for source in ['',str(self.root/'missing'),str(workspace)]:
            self.error('/a/api/workspace/import',400,body={'category':'base','platform':'IOS','version':'9','source':source})
            self.error('/a/api/workspace/import',400,body={'category':'patch','platform':'IOS','source':source})
        self.assertFalse((workspace/'bases'/'IOS'/'9').exists())
        self.assertNotIn('import_sources',json.loads((self.root/'config.json').read_text()))

    def test_workspace_settings_require_server_token_but_import_uses_project_token(self):
        app.PROJECT_REGISTRY['server_admin_token']='server-secret'
        disk=json.loads((self.root/'config.json').read_text());disk['admin_token']='project-secret'
        (self.root/'config.json').write_text(json.dumps(disk))
        workspace=self.root/'workspace'/'a'
        self.error('/a/api/workspace',401,body={'root':str(workspace)},headers={'Authorization':'Bearer project-secret'})
        self.request('/a/api/workspace',{'root':str(workspace)},headers={'Authorization':'Bearer server-secret'})
        self.request('/a/api/dirs/list?path='+str(self.root),headers={'Authorization':'Bearer server-secret'})
        source=self.root/'external';source.mkdir();(source/'game.txt').write_text('base');(source/'zz_base.pak').write_bytes(b'pak')
        self.request('/a/api/workspace/import',{'category':'base','platform':'IOS','version':'4.0','source':str(source)},headers={'Authorization':'Bearer project-secret'})
        self.assertTrue((workspace/'bases'/'IOS'/'4.0'/'game.txt').exists())

    def test_created_projects_use_named_workspace_and_no_inherited_data(self):
        self.request('/a/api/projects', {'key': 'c', 'platforms': ['Windows', 'Android', 'IOS']})
        view = self.request('/c/api/workspace')
        self.assertEqual(Path(view['paths']['root']), self.root / 'workspace' / 'c')
        self.assertTrue((self.root / 'workspace' / 'c' / 'bases' / 'IOS').is_dir())
        self.assertEqual(self.request('/c/api/versions')['versions'], [])
        self.error('/a/api/workspace', 400, body={'root': str(self.root / 'workspace')})
        self.error('/a/api/workspace', 400, body={'root': view['paths']['root']})

    def test_non_supported_channels_are_rejected_by_api(self):
        for platform in ('Linux', 'Android_ASTC', 'Mac'):
            self.error('/a/api/projects/platforms', 400, body={'key': 'a', 'platforms': [platform]})
            self.error('/a/api/projects', 400, body={'key': 'c', 'platforms': [platform]})

    def test_launcher_template_uses_project_workspace(self):
        source = self.root / 'projects' / 'b' / 'data' / 'launcher'
        source.mkdir(parents=True)
        (source / 'launcher_config.json').write_text(json.dumps({'serverUrl': self.url + '/b', 'projectName': 'TemplateB'}))
        self.request('/a/api/projects', {'key': 'c', 'copyFrom': 'b', 'platforms': ['Windows']})
        target = self.root / 'workspace' / 'c' / 'data' / 'launcher' / 'launcher_config.json'
        config = json.loads(target.read_text(encoding='utf-8-sig'))
        self.assertEqual(config['serverUrl'], self.url + '/c')
        self.assertEqual(config['projectName'], 'TemplateB')
        self.assertFalse((self.root / 'projects' / 'c' / 'data').exists())
        self.request('/a/api/projects/rename', {'key': 'c', 'newKey': 'd'})
        self.assertEqual(json.loads(target.read_text(encoding='utf-8-sig'))['serverUrl'], self.url + '/d')

    def test_cloud_template_uses_independent_project_prefix(self):
        path = self.root / 'projects' / 'b' / 'config.json'
        disk = json.loads(path.read_text())
        disk['storage'] = {'backend': 's3', 's3': {'prefix': 'updates'}}
        path.write_text(json.dumps(disk))
        self.request('/a/api/projects', {'key': 'c', 'copyFrom': 'b', 'platforms': ['Windows']})
        new = json.loads((self.root / 'projects' / 'c' / 'config.json').read_text())
        self.assertEqual(new['storage']['s3']['prefix'], 'updates/projects/c')
        self.assertEqual(json.loads(path.read_text())['storage']['s3']['prefix'], 'updates')


if __name__=='__main__':unittest.main()
