import unittest

from config import validate_platforms


class SupportedPlatformTests(unittest.TestCase):
    def test_only_three_canonical_channels(self):
        self.assertEqual(validate_platforms(['windows', 'ANDROID', 'ios']), ['Windows', 'Android', 'IOS'])
        for platforms in (['Linux'], ['Mac'], ['Android_ASTC'], ['androud'], [], ['Windows', 'windows']):
            with self.assertRaises(ValueError):
                validate_platforms(platforms)
