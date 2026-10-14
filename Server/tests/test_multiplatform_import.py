import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from importer import import_version, run_import, base_import_source


class MultiPlatformImportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        self.cfg = {'project': 'Test', 'platforms': ['Windows', 'Android', 'IOS'],
                    'default_platform': 'Windows', 'data_dir': str(self.root / 'data'),
                    'versions_dir': str(self.root / 'library'), 'manifests_dir': str(self.root / 'manifests'),
                    'base_packages': {}, 'platform_settings': {}, 'manifest_exclude_patterns': [], 'manifest_hash': 'md5'}

    def tearDown(self):
        self.tmp.cleanup()

    def release(self, version, platform, contents=b'resource'):
        folder = self.source / version
        (folder / platform).mkdir(parents=True)
        (folder / (version + '_Release.json')).write_text(json.dumps({'baseVersionId': '1.0'}))
        (folder / platform / 'game.pak').write_bytes(contents)
        return folder

    def test_mixed_root_skips_versions_without_selected_platform(self):
        self.release('1.1', 'Windows')
        self.release('1.2', 'Android')
        result = run_import(str(self.source), self.cfg['data_dir'], 'Test', 'Android', config=self.cfg)
        self.assertEqual([v['versionId'] for v in result['versions']], ['1.2'])
        self.assertFalse((self.root / 'library' / 'Android' / '1.1').exists())
        self.assertFalse((self.root / 'library' / 'Windows').exists())

    def test_same_version_and_filename_are_isolated(self):
        self.release('1.1', 'Windows', b'windows')
        self.release('1.1', 'Android', b'android')
        for platform, data in [('Windows', b'windows'), ('Android', b'android')]:
            desc, _ = import_version(str(self.source), self.cfg['versions_dir'], '1.1', platform, '', config=self.cfg)
            self.assertIn('/' + platform + '/', desc['files'][0]['url'])
            self.assertEqual((self.root / 'library' / platform / '1.1' / 'files' / 'game.pak').read_bytes(), data)

    def test_delta_published_once_with_verified_fallback(self):
        folder = self.release('1.1', 'Android', b'full')
        (folder / 'Android' / 'game.pak.patch').write_bytes(b'delta')
        (folder / '1.1_PakFilesInfo.json').write_text(json.dumps({'pakFilesMap': {'Android': {'pakFileInfos': [{'fileName': 'game.pak', 'hash': 'stale', 'fileSize': 999}]}}}))
        desc, _ = import_version(str(self.source), self.cfg['versions_dir'], '1.1', 'Android', '', config=self.cfg)
        self.assertEqual(len(desc['files']), 1)
        entry = desc['files'][0]
        self.assertTrue(entry['binaryPatch'])
        self.assertEqual(entry['kind'], 'ContentPak')
        self.assertEqual((entry['hash'], entry['size']), (hashlib.md5(b'delta').hexdigest(), 5))
        self.assertEqual((entry['fallbackHash'], entry['fallbackSize']), (hashlib.md5(b'full').hexdigest(), 4))
        self.assertIn('/Android/', entry['fallbackUrl'])

    def test_platform_specific_metadata_overrides_shared_metadata(self):
        folder = self.release('1.1', 'Android')
        (folder / 'Android' / '1.1_Release.json').write_text(json.dumps({'baseVersionId': 'android-base', 'requiredGameVersion': '2.0', 'resourceVersion': '10'}))
        desc, _ = import_version(str(self.source), self.cfg['versions_dir'], '1.1', 'Android', '', config=self.cfg)
        self.assertEqual((desc['baseVersionId'], desc['requiredGameVersion'], desc['resourceVersion']), ('android-base', '2.0', '10'))

    def test_single_version_and_platform_local_release(self):
        folder = self.release('1.1', 'Android')
        result = run_import(str(folder), self.cfg['data_dir'], 'Test', 'Android', config=self.cfg)
        self.assertEqual(result['platformUpdateChains']['Android'], ['1.1'])
        (folder / '1.1_Release.json').rename(folder / 'Android' / '1.1_Release.json')
        result = run_import(str(self.source), self.cfg['data_dir'], 'Test', 'Android', config=self.cfg)
        self.assertEqual(result['platformUpdateChains']['Android'], ['1.1'])

    def test_empty_platform_import_preserves_published_version(self):
        folder = self.release('1.1', 'Android')
        import_version(str(self.source), self.cfg['versions_dir'], '1.1', 'Android', '', config=self.cfg)
        (folder / 'Android' / 'game.pak').unlink()
        with self.assertRaises(ValueError):
            import_version(str(self.source), self.cfg['versions_dir'], '1.1', 'Android', '', config=self.cfg)
        self.assertEqual((self.root / 'library' / 'Android' / '1.1' / 'files' / 'game.pak').read_bytes(), b'resource')

    def test_android_base_release_uses_data_subtree_and_rejects_apk_only(self):
        folder = self.root / 'release' / 'Android'
        folder.mkdir(parents=True)
        (folder / 'Game.apk').write_bytes(b'apk')
        with self.assertRaises(ValueError):
            base_import_source(str(self.root / 'release'), 'Android')
        pak = folder / 'Data' / 'Game' / 'Content' / 'Paks' / 'game.pak'
        pak.parent.mkdir(parents=True)
        pak.write_bytes(b'pak')
        self.assertEqual(base_import_source(str(self.root / 'release'), 'Android'), str(folder / 'Data'))

    def test_external_files_use_referenced_base_not_latest(self):
        folder = self.release('1.1', 'Android')
        old, latest = self.root / 'old', self.root / 'latest'
        old.mkdir(); latest.mkdir()
        (old / 'config.ini').write_bytes(b'old-base')
        (latest / 'config.ini').write_bytes(b'new-base')
        self.cfg['base_packages'] = {'Android': {'1.0': str(old), '2.0': str(latest)}}
        (folder / '1.1_Release.json').write_text(json.dumps({'baseVersionId': '1.0', 'platformAssets': {'Android': {'addExternFileToPak': [{'mountPath': '../../../config.ini'}]}}}))
        desc, _ = import_version(str(self.source), self.cfg['versions_dir'], '1.1', 'Android', str(latest), config=self.cfg)
        external = next(f for f in desc['files'] if f['kind'] == 'ExternFile')
        self.assertEqual(external['hash'], hashlib.md5(b'old-base').hexdigest())
        self.assertIn('/Android/1.0/', external['url'])

    def test_iostore_without_patch_config_requires_restart(self):
        folder = self.release('1.1', 'Windows')
        (folder / 'Windows' / 'game.utoc').write_bytes(b'container')
        desc, _ = import_version(str(self.source), self.cfg['versions_dir'], '1.1', 'Windows', '', config=self.cfg)
        self.assertTrue(desc['ioStoreEnabled'])
        self.assertTrue(desc['restartRequired'])

    def test_alias_import_publishes_canonical_android(self):
        folder = self.release('1.1', 'Android_ASTC', b'astc')
        (folder / 'Android_ASTC' / '1.1_Release.json').write_text(json.dumps({'baseVersionId': '1.0', 'requiredGameVersion': '2.0'}))
        desc, _ = import_version(str(self.source), self.cfg['versions_dir'], '1.1', 'Android', '', config=self.cfg)
        self.assertEqual(desc['platform'], 'Android')
        self.assertEqual(desc['requiredGameVersion'], '2.0')
        self.assertIn('/Android/', desc['files'][0]['url'])
        self.assertEqual((self.root / 'library' / 'Android' / '1.1' / 'files' / 'game.pak').read_bytes(), b'astc')

    def test_multiple_cook_formats_require_selection(self):
        folder = self.release('1.1', 'Android_ASTC', b'astc')
        self.release('1.1', 'Android_ETC2', b'etc2')
        with self.assertRaises(ValueError):
            run_import(str(self.source), self.cfg['data_dir'], 'Test', 'Android', config=self.cfg)
        self.assertFalse((self.root / 'library' / 'Android' / '1.1').exists())
        run_import(str(folder / 'Android_ETC2'), self.cfg['data_dir'], 'Test', 'Android', config=self.cfg)
        self.assertEqual((self.root / 'library' / 'Android' / '1.1' / 'files' / 'game.pak').read_bytes(), b'etc2')

    def test_base_alias_and_ios_paths(self):
        root = self.root / 'bases'
        for platform in ('Android_ASTC', 'IOS'):
            target = root / platform / 'Data' / 'Game' / 'Content' / 'Paks'
            target.mkdir(parents=True)
            (target / 'game.pak').write_bytes(platform.encode())
        self.assertEqual(base_import_source(str(root), 'Android'), str(root / 'Android_ASTC' / 'Data'))
        self.assertEqual(base_import_source(str(root), 'IOS'), str(root / 'IOS'))
