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
