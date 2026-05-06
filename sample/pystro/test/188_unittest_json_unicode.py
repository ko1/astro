import unittest
import json


class JsonUnicodeTest(unittest.TestCase):
    def test_ascii_escape_default(self):
        self.assertEqual(json.dumps("café"), '"caf\\u00e9"')

    def test_no_ensure_ascii(self):
        self.assertEqual(json.dumps("café", ensure_ascii=False), '"café"')

    def test_emoji_surrogate_pair(self):
        # CPython encodes BMP-out chars as surrogate pairs in JSON output.
        self.assertEqual(json.dumps("\U0001F600"), '"\\ud83d\\ude00"')

    def test_round_trip_no_ascii(self):
        s = "héllo"
        out = json.dumps(s, ensure_ascii=False)
        self.assertEqual(json.loads(out), s)

    def test_round_trip_ascii(self):
        s = "café"
        out = json.dumps(s)
        self.assertEqual(json.loads(out), s)


unittest.main(globals())
