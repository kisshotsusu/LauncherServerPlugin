import unittest
from versions import add_version_channels, filter_index_by_platform
class VersionChannelTests(unittest.TestCase):
    def test_independent_channels(self):
        source={"versions":[{"versionId":"game-2","gameVersion":"2.0","resourceVersion":"10.0","type":"full"},{"versionId":"12","resourceVersion":"12.0","type":"patch"},{"versionId":"11","resourceVersion":"11.0","type":"patch"}],"updateChain":["12","11"]}
        result=add_version_channels(source)
        self.assertEqual(result["gameVersion"],"2.0")
        self.assertEqual(result["resourceVersion"],"11.0")
        self.assertNotIn("gameVersion",source)
    def test_legacy_and_empty(self):
        self.assertEqual(add_version_channels({})["resourceVersion"],"")
        r=add_version_channels({"versions":[{"versionId":"1.0","type":"full"}]})
        self.assertEqual((r["gameVersion"],r["resourceVersion"]),("1.0","1.0"))
    def test_platforms(self):
        source={"platforms":["Windows","Linux"],"versions":[{"platform":"Windows","versionId":"2.0","type":"full"},{"platform":"Linux","versionId":"9.0","type":"full"}],"baseVersions":{"Windows":["2.0"],"Linux":["9.0"]}}
        self.assertEqual(add_version_channels(filter_index_by_platform(source,"Windows"))["gameVersion"],"2.0")
    def test_hidden_version_not_reintroduced(self):
        self.assertEqual(add_version_channels({"gameVersion":"9.0","versions":[{"versionId":"1.0","type":"full"}]})["gameVersion"],"1.0")
    def test_requested_platform_controls_both_versions_without_global_config(self):
        source={'platforms':['Windows','Android_ASTC','Android_ETC2'], 'gameVersion':'global-must-not-be-used',
                'resourceVersion':'global-must-not-be-used', 'versions':[
                    {'platform':'Windows','versionId':'win-base','type':'full','gameVersion':'2.0','resourceVersion':'10'},
                    {'platform':'Android_ASTC','versionId':'astc-base','type':'full','gameVersion':'3.0','resourceVersion':'20'},
                    {'platform':'Android_ASTC','versionId':'astc-patch','type':'patch','resourceVersion':'21'},
                    {'platform':'Android_ETC2','versionId':'etc-base','type':'full','gameVersion':'4.0','resourceVersion':'30'}],
                'platformUpdateChains':{'Windows':[], 'Android_ASTC':['astc-patch'], 'Android_ETC2':[]}}
        for platform, expected in [('Windows',('2.0','10')), ('Android_ASTC',('3.0','21')), ('Android_ETC2',('4.0','30'))]:
            result=add_version_channels(filter_index_by_platform(source,platform))
            self.assertEqual(result['platform'],platform)
            self.assertEqual((result['gameVersion'],result['resourceVersion']),expected)
        self.assertEqual(source['gameVersion'],'global-must-not-be-used')
