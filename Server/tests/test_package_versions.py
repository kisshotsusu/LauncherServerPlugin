import json
import sys
import unittest
import zipfile
import plistlib
from unittest.mock import patch
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import test_platform_http as fixtures
from test_import_policy import write_apk


class PackageVersionHTTPTests(unittest.TestCase):
    setUp = fixtures.PlatformHTTPTests.setUp
    tearDown = fixtures.PlatformHTTPTests.tearDown
    request = fixtures.PlatformHTTPTests.request
    error = fixtures.PlatformHTTPTests.error

    def workspace(self, project='a'):
        self.request('/a/api/projects/platforms', {'key': project, 'platforms': ['Windows', 'Android', 'IOS']})
        root = self.root / 'workspace' / project
        self.request('/' + project + '/api/workspace', {'root': str(root)})
        return root

    def import_both(self, project='a', version='2.0', bundled='10.0'):
        root = self.workspace(project)
        source = self.root / (project + '-release')
        source.mkdir()
        write_apk(source / 'Game.apk')
        resources = source / 'Data'; resources.mkdir()
        (resources / 'game.pak').write_bytes(b'pak')
        self.request('/' + project + '/api/workspace/import', {'category':'apk','platform':'Android','version':version,'source':str(source / 'Game.apk'),'bundledResourceVersion':bundled})
        self.request('/' + project + '/api/workspace/import', {'category':'base','platform':'Android','version':version,'source':str(resources),'requiredGameVersion':version})
        return root

    def test_same_id_installer_and_resources_have_distinct_descriptors(self):
        self.import_both()
        index = self.request('/a/Android/api/versions')
        self.assertEqual(index['gameVersion'], '2.0')
        self.assertEqual(index['resourceVersion'], '10.0')
        entries = {v['packageKind']:v for v in index['versions']}
        self.assertEqual(set(entries), {'installer','base'})
        self.assertIn('kind=installer', entries['installer']['url'])
        self.assertIn('kind=base', entries['base']['url'])
        app = self.request(entries['installer']['url'])
        base = self.request(entries['base']['url'])
        self.assertEqual(app['type'], 'full')
        self.assertEqual(base['type'], 'resource')
        self.assertEqual(base['requiredGameVersion'], '2.0')
        self.assertEqual(base['files'][0]['kind'], 'ContentPak')
        self.assertEqual(self.request(app['files'][0]['url'], raw=True)[:2], b'PK')
        self.assertEqual(self.request(base['files'][0]['url'], raw=True), b'pak')

    def test_deleting_resource_base_preserves_same_id_installer(self):
        root = self.import_both()
        self.error('/a/Android/api/version/2.0',400,method='DELETE')
        self.request('/a/Android/api/version/2.0?kind=base', method='DELETE')
        self.assertFalse((root / 'bases' / 'Android' / '2.0').exists())
        self.assertTrue((root / 'installers' / 'Android' / '2.0' / 'Game.apk').exists())
        self.assertTrue(list((root / 'data' / 'trash' / 'Android' / 'base').glob('2.0_*')))
        index = self.request('/a/Android/api/versions')
        self.assertEqual([v['packageKind'] for v in index['versions']], ['installer'])
        self.assertTrue(any(r['packageKind']=='base' for r in index['revoked']))

    def test_deleting_installer_preserves_resource_base_and_channel(self):
        root = self.import_both(bundled='2.0')
        self.request('/a/Android/api/version/2.0?kind=installer',method='DELETE')
        self.assertTrue((root / 'bases' / 'Android' / '2.0' / 'game.pak').exists())
        self.assertTrue(list((root / 'data' / 'trash' / 'Android' / 'installer').glob('2.0_*')))
        index = self.request('/a/Android/api/versions')
        self.assertEqual((index['gameVersion'],index['resourceVersion']), ('','2.0'))

    def test_invalid_legacy_base_is_visible_to_admin_and_removable(self):
        root = self.workspace()
        folder=root/'bases'/'Android'/'bad'; folder.mkdir()
        (folder/'old.obb').write_bytes(b'keep')
        self.request('/a/api/versions/reindex',{})
        admin=self.request('/a/Android/api/versions?all=1')
        self.assertEqual(admin['invalidVersions'][0]['versionId'],'bad')
        self.assertFalse(self.request('/a/Android/api/versions')['versions'])
        self.request('/a/Android/api/version/bad?kind=base',method='DELETE')
        self.assertFalse(folder.exists())
        self.assertTrue(list((root/'data'/'trash'/'Android'/'base').glob('bad_*/old.obb')))

    def test_cloud_files_separate_apk_and_resources_for_same_version(self):
        self.import_both()
        apk=self.request('/a/Android/api/files?kind=installer&baseVersion=2.0')
        base=self.request('/a/Android/api/files?kind=base&baseVersion=2.0')
        self.assertEqual([f['name'] for f in apk['entries']], ['Game.apk'])
        self.assertEqual([f['name'] for f in base['entries']], ['game.pak'])
        self.assertEqual(self.request(apk['entries'][0]['downloadUrl'],raw=True)[:2],b'PK')
        self.error('/a/Windows/api/files?kind=installer&baseVersion=2.0',404)

    def test_application_and_resource_switches_do_not_share_version_id(self):
        self.import_both(bundled='2.0')
        self.request('/a/Android/api/enabled_versions',{'versions':['installer:2.0']})
        index=self.request('/a/Android/api/versions')
        self.assertEqual([v['packageKind'] for v in index['versions']],['installer'])
        self.assertEqual(index['baseVersions']['Android'],[])
        self.request('/a/Android/api/enabled_versions',{'versions':['base:2.0']})
        index=self.request('/a/Android/api/versions')
        self.assertEqual([v['packageKind'] for v in index['versions']],['base'])
        self.assertEqual((index['gameVersion'],index['resourceVersion']),('','2.0'))

    def test_independent_resource_version_and_game_dependency(self):
        root=self.workspace()
        source=self.root/'data-source';source.mkdir();(source/'game.pak').write_bytes(b'pak')
        self.request('/a/api/workspace/import',{'category':'base','platform':'Android','version':'10.0','source':str(source),'requiredGameVersion':'2.0'})
        index=self.request('/a/Android/api/versions')
        self.assertEqual((index['gameVersion'],index['resourceVersion']),('','10.0'))
        self.assertEqual(index['versions'][0]['requiredGameVersion'],'2.0')
        self.assertNotIn('.cloudupdate.json',[e['name'] for e in self.request('/a/Android/api/files?baseVersion=10.0')['entries']])

    def test_windows_external_base_unregisters_without_moving_source(self):
        source=self.root/'base'/'Windows'
        self.request('/a/Windows/api/version/1.0?kind=base',method='DELETE')
        self.assertTrue(source.is_dir())
        self.error('/a/Windows/api/version/1.0?kind=base',404)
        self.assertEqual(self.request('/b/Windows/api/version/1.0?kind=base')['versionId'],'1.0')

    def import_platform_pair(self, platform):
        root = self.workspace()
        source = self.root / 'program'; source.mkdir()
        resources = self.root / 'resources'; resources.mkdir()
        (resources / 'game.pak').write_bytes(b'pak')
        if platform == 'Windows':
            (source / 'Game.exe').write_bytes(b'MZprogram')
            binaries = source / 'Game' / 'Binaries' / 'Win64'; binaries.mkdir(parents=True)
            (binaries / 'runtime.dll').write_bytes(b'MZdll')
            (source / 'empty.ini').write_bytes(b'')
            (source / 'game.pak').write_bytes(b'bundled')
        else:
            archive = source / 'Game.ipa'
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('Payload/Game.app/Info.plist', plistlib.dumps({'CFBundleExecutable':'Game'}))
                z.writestr('Payload/Game.app/Game', b'program')
            source = archive
        for category, path in [('application',source),('base',resources)]:
            self.request('/a/api/workspace/inspect', {'category':category,'platform':platform,'source':str(path)})
            self.request('/a/api/workspace/import', {'category':category,'platform':platform,'source':str(path),'version':'2.0','bundledResourceVersion':'1.5','requiredGameVersion':'2.0'})
        return root

    def test_windows_program_and_resources_are_independent(self):
        root = self.import_platform_pair('Windows')
        index = self.request('/a/Windows/api/versions')
        self.assertEqual((index['gameVersion'],index['resourceVersion']), ('2.0','2.0'))
        app = self.request('/a/Windows/api/version/2.0?kind=installer')
        resource = self.request('/a/Windows/api/version/2.0?kind=base')
        self.assertEqual(app['type'],'full')
        self.assertEqual(app['resourceVersion'],'1.5')
        self.assertEqual(resource['type'],'resource')
        self.assertEqual(resource['files'][0]['kind'],'ContentPak')
        self.assertEqual({f['targetRelativePath'] for f in app['files']},{'Game.exe','Game/Binaries/Win64/runtime.dll','empty.ini','game.pak'})
        dll = next(f for f in app['files'] if f['fileName']=='runtime.dll')
        self.assertEqual(self.request(dll['url'],raw=True),b'MZdll')
        cloud = self.request('/a/Windows/api/files?kind=installer&baseVersion=2.0&path=Game/Binaries/Win64')
        self.assertEqual(cloud['entries'][0]['name'],'runtime.dll')
        self.assertEqual(self.request(cloud['entries'][0]['downloadUrl'],raw=True),b'MZdll')
        self.request('/a/Windows/api/enabled_versions',{'versions':['installer:2.0']})
        self.assertEqual([v['packageKind'] for v in self.request('/a/Windows/api/versions')['versions']],['installer'])
        self.request('/a/Windows/api/version/2.0?kind=installer',method='DELETE')
        self.assertTrue((root/'bases'/'Windows'/'2.0'/'game.pak').exists())
        self.assertTrue(list((root/'data'/'trash'/'Windows'/'installer').glob('2.0_*/Game.exe')))

    def test_ios_program_and_resources_are_independent(self):
        root = self.import_platform_pair('IOS')
        index = self.request('/a/IOS/api/versions')
        self.assertEqual((index['gameVersion'],index['resourceVersion']),('2.0','2.0'))
        app = self.request('/a/IOS/api/version/2.0?kind=installer')
        cloud = self.request('/a/IOS/api/files?kind=installer&baseVersion=2.0')
        self.assertEqual(cloud['entries'][0]['name'],'Game.ipa')
        self.assertEqual(self.request(app['files'][0]['url'],raw=True)[:2],b'PK')
        self.request('/a/IOS/api/version/2.0?kind=base',method='DELETE')
        self.assertTrue((root/'installers'/'IOS'/'2.0'/'Game.ipa').exists())

    def test_rejects_wrong_or_incomplete_program_sources_before_publication(self):
        root=self.workspace()
        source=self.root/'not-program';source.mkdir()
        (source/'game.pak').write_bytes(b'resources')
        for platform in ['Windows','IOS','Android']:
            self.error('/a/api/workspace/import',400,body={'category':'application','platform':platform,'source':str(source),'version':'9'})
            self.assertFalse((root/'installers'/platform/'9').exists())
        (source/'fake.ipa').write_bytes(b'broken')
        self.error('/a/api/workspace/inspect',400,body={'category':'application','platform':'IOS','source':str(source/'fake.ipa')})

    def test_remote_program_upload_failure_does_not_publish_local_version(self):
        root = self.workspace()
        source = self.root/'Game.apk'; write_apk(source)
        with patch('run_server.get_storage') as storage:
            storage.return_value.is_remote = True
            storage.return_value.upload_file.return_value = False
            self.error('/a/api/workspace/import',400,body={'category':'application','platform':'Android','source':str(source),'version':'9'})
            call = storage.return_value.upload_file.call_args
            self.assertEqual(call.args,('installers',))
            self.assertEqual((call.kwargs['platform'],call.kwargs['version'],call.kwargs['rel']),('Android','9','Game.apk'))
        self.assertFalse((root/'installers'/'Android'/'9').exists())

    def test_remote_resource_upload_failure_does_not_publish_local_version(self):
        root = self.workspace()
        source = self.root/'resources'; source.mkdir(); (source/'game.pak').write_bytes(b'pak')
        with patch('run_server.get_storage') as storage:
            storage.return_value.is_remote = True
            storage.return_value.upload_file.return_value = False
            self.error('/a/api/workspace/import',400,body={'category':'base','platform':'Windows','source':str(source),'version':'9'})
            self.assertEqual(storage.return_value.upload_file.call_args.args,('packages',))
        self.assertFalse((root/'bases'/'Windows'/'9').exists())

    def test_fallback_urls_keep_the_selected_project(self):
        for project in ('a','b'):
            descriptor = self.root / ('library' if project=='a' else 'projects/b/library') / 'Windows' / '1.1' / 'descriptor.json'
            desc = json.loads(descriptor.read_text())
            desc['files'][0]['fallbackUrl']='/files/packages/Windows/1.0/zz_base.pak'
            descriptor.write_text(json.dumps(desc))
            result=self.request('/'+project+'/Windows/api/version/1.1?kind=patch')
            self.assertTrue(result['files'][0]['fallbackUrl'].startswith('/'+project+'/files/'))
            self.assertEqual(self.request(result['files'][0]['fallbackUrl'],raw=True),b'pak')

    def test_empty_legacy_full_descriptor_is_not_a_resource_update(self):
        folder=self.root/'library'/'Windows'/'9.0';folder.mkdir()
        (folder/'descriptor.json').write_text(json.dumps({'versionId':'9.0','platform':'Windows','type':'full','files':[]}))
        self.request('/a/api/versions/reindex',{})
        client=self.request('/a/Windows/api/versions')
        self.assertNotIn('9.0',[v['versionId'] for v in client['versions']])
        admin=self.request('/a/Windows/api/versions?all=1')
        self.assertTrue(any(v['versionId']=='9.0' for v in admin['invalidVersions']))
        self.request('/a/Windows/api/version/9.0?kind=patch',method='DELETE')
        self.assertFalse(folder.exists())

    def test_cloud_resource_files_match_the_published_manifest(self):
        self.workspace()
        source=self.root/'resources';source.mkdir()
        (source/'game.pak').write_bytes(b'pak')
        (source/'image.png').write_bytes(b'image')
        (source/'Manifest_DebugFiles_Android.txt').write_bytes(b'debug')
        self.request('/a/api/config/update',{'manifestExcludePatterns':['Manifest_*.txt']})
        self.request('/a/api/workspace/import',{'category':'base','platform':'Android','source':str(source),'version':'9'})
        cloud=self.request('/a/Android/api/files?kind=base&baseVersion=9')
        desc=self.request('/a/Android/api/version/9?kind=base')
        self.assertEqual({f['name'] for f in cloud['entries']},{f['targetRelativePath'] for f in desc['files']})
        self.assertEqual({f['name'] for f in cloud['entries']},{'game.pak','image.png'})
        self.error('/a/files/packages/Android/9/Manifest_DebugFiles_Android.txt',400)

    def upload(self, path, name, content):
        boundary = 'CloudUpdateTestBoundary'
        body = ('--'+boundary+'\r\nContent-Disposition: form-data; name="file"; filename="'+name+'"\r\nContent-Type: application/octet-stream\r\n\r\n').encode()+content+('\r\n--'+boundary+'--\r\n').encode()
        req=Request(self.url+path,data=body,headers={'Content-Type':'multipart/form-data; boundary='+boundary})
        with urlopen(req,timeout=5) as response: return json.load(response)

    def test_resource_directory_upload_updates_real_files_and_manifest(self):
        self.workspace()
        source=self.root/'resources';source.mkdir();(source/'game.pak').write_bytes(b'pak')
        self.request('/a/api/workspace/import',{'category':'base','platform':'Windows','source':str(source),'version':'9'})
        result=self.upload('/a/Windows/api/upload?target=package&baseVersion=9&kind=base','image.png',b'image')
        self.assertTrue(result['ok'])
        cloud=self.request('/a/Windows/api/files?baseVersion=9')
        self.assertIn('image.png',[f['name'] for f in cloud['entries']])
        manifest=self.request('/a/Windows/api/manifest.json?baseVersion=9')
        self.assertIn('image.png',[f['path'] for f in manifest['files']])
        self.assertEqual(self.request('/a/files/packages/Windows/9/image.png',raw=True),b'image')

    def test_resource_upload_rejects_native_programs_on_every_platform(self):
        self.workspace()
        for platform, name in [('Windows','Game.exe'),('Android','Game.apk'),('IOS','Game.ipa')]:
            with self.assertRaises(HTTPError) as error:
                self.upload('/a/'+platform+'/api/upload?target=package&baseVersion=1.0',name,b'program')
            self.assertEqual(error.exception.code,400)
