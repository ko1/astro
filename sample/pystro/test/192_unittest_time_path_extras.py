import unittest
import time
import os


class TimeFormatTest(unittest.TestCase):
    def test_strftime_year(self):
        t = time.localtime()
        out = time.strftime("%Y", t)
        self.assertTrue(int(out) >= 2025)

    def test_strftime_iso_date(self):
        t = time.localtime()
        out = time.strftime("%Y-%m-%d", t)
        self.assertEqual(len(out), 10)
        self.assertEqual(out[4], "-")
        self.assertEqual(out[7], "-")

    def test_localtime_attrs(self):
        t = time.localtime()
        self.assertGreaterEqual(t.tm_year, 2025)
        self.assertGreaterEqual(t.tm_mon, 1)
        self.assertLessEqual(t.tm_mon, 12)
        self.assertGreaterEqual(t.tm_mday, 1)


class OsPathExpandTest(unittest.TestCase):
    def test_expanduser(self):
        home = os.environ.get("HOME", "/")
        self.assertEqual(os.path.expanduser("~/foo"), home + "/foo")

    def test_expanduser_no_tilde(self):
        self.assertEqual(os.path.expanduser("/abs/path"), "/abs/path")

    def test_expandvars(self):
        os.environ_set = lambda k, v: None  # placeholder
        self.assertEqual(os.path.expandvars("$HOME/x"),
                         os.environ.get("HOME") + "/x")

    def test_expandvars_braces(self):
        self.assertEqual(os.path.expandvars("${HOME}/x"),
                         os.environ.get("HOME") + "/x")

    def test_expandvars_unknown(self):
        self.assertEqual(os.path.expandvars("$NOPE_ZZZZZ/x"),
                         "$NOPE_ZZZZZ/x")


unittest.main(globals())
