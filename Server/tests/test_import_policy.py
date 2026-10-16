import json
import shutil
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import test_platform_http as fixtures
from importer import inspect_base_source, find_android_apk, iter_resource_files, validate_resource_tree, patch_import_plan, run_import


def write_apk(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w') as z:
        z.writestr('AndroidManifest.xml', b'manifest')
        z.writestr('classes.dex', b'dex')


class ImportPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def resource(self, root):
        pak = root / 'Game' / 'Content' / 'Paks' / 'game.pak'
        pak.parent.mkdir(parents=True, exist_ok=True)
        pak.write_bytes(b'pak')

    def test_apk_selection_never_guesses_among_multiple_files(self):
        write_apk(self.root / 'Game-arm64.apk')
        write_apk(self.root / 'Game-test.apk')
        with self.assertRaises(ValueError): find_android_apk(str(self.root))
        self.assertEqual(find_android_apk(str(self.root / 'Game-test.apk')), str(self.root / 'Game-test.apk'))

    def test_mixed_release_scopes_apk_to_selected_android_folder(self):
        self.resource(self.root / 'Android_ASTC' / 'Data')
        write_apk(self.root / 'Android_ASTC' / 'Game.apk')
        write_apk(self.root / 'Windows' / 'wrong.apk')
        result = inspect_base_source(str(self.root), 'Android')
        self.assertEqual(result['apk'], str(self.root / 'Android_ASTC' / 'Game.apk'))
        self.assertEqual(result['source'], str(self.root / 'Android_ASTC' / 'Data'))

    def test_explicit_wrong_platform_and_ambiguous_flavors_are_rejected(self):
        self.resource(self.root / 'Windows')
        with self.assertRaises(ValueError): inspect_base_source(str(self.root / 'Windows'), 'Android')
        self.resource(self.root / 'Android')
        self.resource(self.root / 'Android_ASTC')
        with self.assertRaises(ValueError): inspect_base_source(str(self.root), 'Android')

    def test_iostore_pair_must_have_same_relative_directory(self):
        for folder in ('a', 'b'): (self.root / folder).mkdir()
        (self.root / 'a' / 'global.utoc').write_bytes(b'index')
        (self.root / 'b' / 'global.ucas').write_bytes(b'data')
        with self.assertRaises(ValueError): validate_resource_tree(str(self.root))

    def test_orphan_ucas_rejected_even_when_pak_exists(self):
        self.resource(self.root)
        (self.root / 'orphan.ucas').write_bytes(b'data')
        with self.assertRaises(ValueError): validate_resource_tree(str(self.root))

    def test_ios_app_extracts_only_cookeddata_and_retains_runtime_assets(self):
        data = self.root / 'Payload' / 'Game.app' / 'cookeddata'
        self.resource(data)
        for filename in ('shader.metallib', 'shader.ushaderbytecode', 'movie.mp4', 'icon.png', 'data.bin'):
            (data / filename).write_bytes(b'runtime')
        (self.root / 'Payload' / 'Game.app' / 'Game').write_bytes(b'native')
        result = inspect_base_source(str(self.root), 'IOS')
        self.assertEqual(result['source'], str(data))
        kept = [rel for _, rel in iter_resource_files(result['source'])]
        self.assertIn('shader.metallib', kept)
        self.assertIn('shader.ushaderbytecode', kept)
        self.assertIn('movie.mp4', kept)
        self.assertIn('icon.png', kept)
        self.assertNotIn('Game', kept)

    def test_windows_resource_import_excludes_program_binaries(self):
        self.resource(self.root)
        (self.root / 'Game.exe').write_bytes(b'exe')
        (self.root / 'Engine.dll').write_bytes(b'dll')
        result = inspect_base_source(str(self.root), 'Windows')
        self.assertEqual(result['baseKind'], 'resources')
        self.assertEqual(result['source'], str(self.root))
        kept = [rel for _, rel in iter_resource_files(result['source'])]
        self.assertNotIn('Game.exe', kept)
        self.assertNotIn('Engine.dll', kept)
        self.assertTrue(any(rel.endswith('.pak') for rel in kept))

    def test_android_archive_with_windows_tools_is_not_windows_game(self):
        write_apk(self.root / 'Game.apk')
        tools = self.root / 'win-x64'; tools.mkdir()
        (tools / 'AndroidFileTool.exe').write_bytes(b'tool')
        with self.assertRaises(ValueError): inspect_base_source(str(self.root), 'Windows')

    def test_metadata_and_cooked_filename_must_match_selected_platform(self):
        self.resource(self.root)
        (self.root / 'version.json').write_text(json.dumps({'platform': 'Windows'}))
        with self.assertRaises(ValueError): inspect_base_source(str(self.root), 'IOS')
        (self.root / 'version.json').unlink()
        (self.root / 'pakchunk0-Windows.pak').write_bytes(b'pak')
        with self.assertRaises(ValueError): inspect_base_source(str(self.root), 'Android')

    def test_patch_version_folder_with_local_release_is_detected(self):
        folder = self.root / '1.1' / 'IOS'; folder.mkdir(parents=True)
        (folder / '1.1_Release.json').write_text(json.dumps({'baseVersionId': '1.0'}))
        (folder / 'game.pak').write_bytes(b'pak')
        _, versions, _ = patch_import_plan(str(folder.parent), 'IOS')
        self.assertEqual(versions, ['1.1'])

    def test_invalid_later_patch_is_detected_before_any_version_is_published(self):
        for version in ('1.1', '1.2'):
            folder = self.root / 'src' / version / 'IOS'; folder.mkdir(parents=True)
            (folder / (version + '_Release.json')).write_text(json.dumps({'baseVersionId': '1.0'}))
            (folder / 'game.pak').write_bytes(b'pak' if version == '1.1' else b'')
        cfg = {'platforms': ['IOS'], 'data_dir': str(self.root / 'data'), 'versions_dir': str(self.root / 'patches'), 'base_packages': {}}
        with self.assertRaises(ValueError): run_import(str(self.root / 'src'), cfg['data_dir'], 'Test', 'IOS', config=cfg)
        self.assertFalse((self.root / 'patches' / 'IOS' / '1.1').exists())


class ImportPolicyHTTPTests(unittest.TestCase):
    setUp = fixtures.PlatformHTTPTests.setUp
    tearDown = fixtures.PlatformHTTPTests.tearDown
    request = fixtures.PlatformHTTPTests.request
    error = fixtures.PlatformHTTPTests.error

    def workspace(self):
        self.request('/a/api/projects/platforms', {'key': 'a', 'platforms': ['Windows', 'Android', 'IOS']})
        root = self.root / 'workspace' / 'a'
        self.request('/a/api/workspace', {'root': str(root)})
        return root

    def release(self):
        root = self.root / 'release'
        pak = root / 'Data' / 'Game' / 'Content' / 'Paks' / 'game.pak'
        pak.parent.mkdir(parents=True)
        pak.write_bytes(b'pak')
        write_apk(root / 'Game.apk')
        return root

    def test_capabilities_and_installation_dirs_are_platform_specific(self):
        root = self.workspace()
        view = self.request('/a/api/workspace')
        for item in view['platforms']:
            if item['platform'] == 'Android':
                self.assertIn('apkImport', item)
                self.assertTrue(item['capabilities']['apkInstaller'])
            else:
                for key in ('apkImport', 'pendingApks'): self.assertNotIn(key, item)
                self.assertTrue((root / 'installers' / item['platform']).exists())
                self.assertFalse((root / 'imports' / item['platform'] / 'apk').exists())
                self.assertEqual(self.request('/a/' + item['platform'] + '/api/installers')['installers'], [])
            self.assertIn('applicationImport', item)
            self.assertTrue(item['capabilities']['applicationPackage'])
            self.assertEqual(item['capabilities']['baseKind'], 'resources')
        self.error('/a/Android/api/launcher/versions', 400)
        self.error('/a/IOS/api/launcher/versions', 400)

    def test_mobile_only_project_has_no_windows_launcher_directories(self):
        self.request('/a/api/projects', {'key': 'mobile', 'platforms': ['Android', 'IOS']})
        root = self.root / 'workspace' / 'mobile'
        self.assertFalse((root / 'launcher').exists())
        self.assertFalse((root / 'imports' / 'launcher').exists())

    def test_preflight_is_read_only_and_rejects_wrong_platform(self):
        root = self.workspace(); release = self.release()
        response = self.request('/a/api/workspace/inspect', {'category': 'base', 'platform': 'Android', 'source': str(release)})
        self.assertEqual(response['inspection']['resourceStats']['pakCount'], 1)
        self.assertEqual(response['inspection']['apk'], 'Game.apk')
        self.assertFalse(any((root / 'bases' / 'Android').iterdir()))
        self.assertNotIn('import_sources', json.loads((self.root / 'config.json').read_text()))
        self.error('/a/api/workspace/inspect', 400, body={'category': 'apk', 'platform': 'Windows', 'source': str(release)})

    def test_invalid_auto_apk_does_not_publish_a_resource_base(self):
        root = self.workspace(); release = self.release()
        (release / 'Game.apk').write_bytes(b'bad')
        self.error('/a/api/workspace/import', 400, body={'category': 'base', 'platform': 'Android', 'source': str(release), 'version': '9'})
        self.assertFalse((root / 'bases' / 'Android' / '9').exists())
        self.assertFalse((root / 'installers' / 'Android' / '9').exists())

    def test_apk_copy_failure_leaves_neither_base_nor_installer(self):
        root = self.workspace(); release = self.release(); original = shutil.copy2
        def copy(src, dst, **kw):
            if str(src).lower().endswith('.apk'): raise OSError('test copy failure')
            return original(src, dst, **kw)
        with patch('run_server.shutil.copy2', side_effect=copy):
            self.error('/a/api/workspace/import', 400, body={'category': 'base', 'platform': 'Android', 'source': str(release), 'version': '9'})
        self.assertEqual(list((root / 'bases' / 'Android').iterdir()), [])
        self.assertEqual(list((root / 'installers' / 'Android').iterdir()), [])

    def test_existing_apk_version_cannot_silently_pair_with_different_resource_base(self):
        root = self.workspace(); release = self.release()
        self.request('/a/api/workspace/import', {'category': 'apk', 'platform': 'Android', 'source': str(release / 'Game.apk'), 'version': '9'})
        self.error('/a/api/workspace/import', 400, body={'category': 'base', 'platform': 'Android', 'source': str(release), 'version': '9'})
        self.assertFalse((root / 'bases' / 'Android' / '9').exists())

    def test_sources_cannot_read_managed_files_from_another_project(self):
        self.workspace()
        other = self.root / 'workspace' / 'b'
        self.request('/b/api/workspace', {'root': str(other)})
        pak = other / 'bases' / 'Windows' / '9' / 'game.pak'; pak.parent.mkdir()
        pak.write_bytes(b'pak')
        self.error('/a/api/workspace/inspect', 400, body={'category': 'base', 'platform': 'Windows', 'source': str(pak.parent)})

    def test_installer_urls_include_project_and_cannot_serve_metadata(self):
        self.workspace(); release = self.release()
        self.request('/a/api/workspace/import', {'category': 'apk', 'platform': 'Android', 'source': str(release / 'Game.apk'), 'version': '9'})
        item = self.request('/a/Android/api/installers')['installers'][0]
        self.assertTrue(item['url'].startswith('/a/files/installers/Android/'))
        self.assertEqual(self.request(item['url'], raw=True)[:2], b'PK')
        self.error('/a/files/installers/Android/9/installer.json', 404)
        self.error('/a/Windows/files/installers/Android/9/Game.apk', 400)

    def test_installer_descriptor_cannot_escape_version_directory(self):
        root = self.workspace()
        folder = root / 'installers' / 'Android' / '9'; folder.mkdir()
        write_apk(self.root / 'outside.apk')
        (folder / 'installer.json').write_text(json.dumps({'platform': 'Android', 'versionId': '9', 'fileName': str(self.root / 'outside.apk')}))
        self.assertEqual(self.request('/a/Android/api/installers')['installers'], [])

    def test_legacy_installer_only_base_is_flagged_without_deleting_files(self):
        root = self.workspace()
        folder = root / 'bases' / 'Android' / '9'; folder.mkdir()
        write_apk(folder / 'Game.apk')
        index_path = root / 'data' / 'versions.json'
        index = json.loads(index_path.read_text()); index.pop('importPolicyVersion', None)
        index_path.write_text(json.dumps(index))
        versions = self.request('/a/Android/api/versions')
        self.assertNotIn('9', [v['versionId'] for v in versions['versions']])
        workspace = self.request('/a/api/workspace')
        android = next(p for p in workspace['platforms'] if p['platform'] == 'Android')
        self.assertFalse(android['baseValidation']['9']['valid'])
        self.error('/a/Android/api/manifest.json?baseVersion=9', 400)
        self.error('/a/Android/api/version/9', 400)
        self.error('/a/files/packages/Android/9/Game.apk', 400)
        self.assertTrue((folder / 'Game.apk').exists())

    def test_legacy_mobile_manifest_filters_installers_and_refreshes_old_cache(self):
        root = self.workspace()
        folder = root / 'bases' / 'Android' / '9'; folder.mkdir()
        (folder / 'game.pak').write_bytes(b'pak')
        (folder / 'image.png').write_bytes(b'asset')
        write_apk(folder / 'Game.apk')
        old_cache = root / 'data' / 'manifests' / 'a_Android_9.json'
        old_cache.write_text(json.dumps({'baseVersionId': '9', 'files': [{'path': 'Game.apk'}]}))
        manifest = self.request('/a/Android/api/manifest.json?baseVersion=9')
        self.assertEqual([f['path'] for f in manifest['files']], ['game.pak', 'image.png'])
        self.assertEqual(manifest['resourcePolicyVersion'], 2)

    def test_resource_publish_failure_rolls_back_auto_installer(self):
        root = self.workspace(); release = self.release()
        original = __import__('os').replace
        def replace(src, dst):
            if str(dst) == str(root / 'bases' / 'Android' / '9'): raise OSError('test publish failure')
            return original(src, dst)
        with patch('run_server.os.replace', side_effect=replace):
            self.error('/a/api/workspace/import', 400, body={'category': 'base', 'platform': 'Android', 'source': str(release), 'version': '9'})
        self.assertEqual(list((root / 'bases' / 'Android').iterdir()), [])
        self.assertEqual(list((root / 'installers' / 'Android').iterdir()), [])

    def test_apk_picker_lists_files_without_changing_directory_only_default(self):
        self.workspace(); release = self.release()
        from urllib.parse import quote
        url = '/a/api/dirs/list?path=' + quote(str(release))
        self.assertFalse(any(e['name'] == 'Game.apk' for e in self.request(url)['entries']))
        entries = self.request(url + '&extensions=.apk')['entries']
        self.assertEqual(next(e for e in entries if e['name'] == 'Game.apk')['isDir'], False)

    def test_empty_patch_import_folder_is_not_reported_as_ready(self):
        self.workspace()
        current = self.request('/a/Android/api/platform/config')['config']
        self.assertTrue(current['sourceExists'])
        self.assertFalse(current['sourceReady'])
        source = self.root / 'patch-source'
        folder = source / '1.1' / 'Android'; folder.mkdir(parents=True)
        (folder / '1.1_Release.json').write_text(json.dumps({'baseVersionId': '1.0'}))
        (folder / 'game.pak').write_bytes(b'patch')
        self.request('/a/api/workspace/import', {'category': 'patch', 'platform': 'Android', 'source': str(source)})
        current = self.request('/a/Android/api/platform/config')['config']
        self.assertTrue(current['sourceReady'])
        self.assertEqual(current['patchSourceDir'], str(source))

    def test_invalid_legacy_base_can_be_repaired_with_same_version_and_backup(self):
        root = self.workspace()
        old = root / 'bases' / 'Android' / '9'; old.mkdir()
        (old / 'archive.obb').write_bytes(b'keep old data')
        release = self.release()
        self.request('/a/api/workspace/import', {'category': 'base', 'platform': 'Android', 'source': str(release / 'Data'), 'version': '9'})
        self.assertTrue((old / 'Game' / 'Content' / 'Paks' / 'game.pak').exists())
        backups = list((root / 'data' / 'trash' / 'invalid-bases' / 'Android').glob('9-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / 'archive.obb').read_bytes(), b'keep old data')
        self.error('/a/api/workspace/import', 409, body={'category': 'base', 'platform': 'Android', 'source': str(release / 'Data'), 'version': '9'})
