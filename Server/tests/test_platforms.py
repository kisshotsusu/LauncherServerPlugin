import json
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from versions import build_versions_index, filter_index_by_platform
from storage import StorageBackend

class PlatformIndexTests(unittest.TestCase):
    def test_platform_orders_are_independent(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            cfg={"project":"Test","platforms":["Windows","Linux"],"default_platform":"Windows","data_dir":d,"versions_dir":str(root/"versions"),"manifests_dir":str(root/"manifests"),"base_packages":{},"platform_settings":{"Windows":{"hotpatcherOrder":"1.2,1.1"},"Linux":{"hotpatcherOrder":"1.1,1.2"}}}
            for pf in cfg["platforms"]:
                for version in ["1.1","1.2"]:
                    target=root/"versions"/pf/version;target.mkdir(parents=True)
                    (target/"descriptor.json").write_text(json.dumps({"platform":pf,"versionId":version,"type":"patch","date":version,"files":[]}))
            index=build_versions_index(cfg)
            self.assertEqual(filter_index_by_platform(index,"Windows")["updateChain"],["1.2","1.1"])
            self.assertEqual(filter_index_by_platform(index,"Linux")["updateChain"],["1.1","1.2"])
    def test_remote_patch_keys_include_platform(self):
        storage=StorageBackend()
        windows=storage._storage_key("versions",platform="Windows",version="1.1",rel="files/game.pak")
        linux=storage._storage_key("versions",platform="Linux",version="1.1",rel="files/game.pak")
        self.assertEqual(windows,"versions/Windows/1.1/files/game.pak")
        self.assertNotEqual(windows,linux)

if __name__=="__main__":unittest.main()
