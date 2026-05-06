import unittest
import os


class OSPathTest(unittest.TestCase):
    def test_normpath(self):
        self.assertEqual(os.path.normpath("/foo/../bar"), "/bar")
        self.assertEqual(os.path.normpath("a/b/../c"), "a/c")
        self.assertEqual(os.path.normpath("./a/./b"), "a/b")
        self.assertEqual(os.path.normpath(""), ".")
        self.assertEqual(os.path.normpath("/"), "/")

    def test_join(self):
        self.assertEqual(os.path.join("/foo", "bar"), "/foo/bar")
        self.assertEqual(os.path.join("/foo", "/bar"), "/bar")  # absolute resets

    def test_split(self):
        self.assertEqual(os.path.split("/usr/local/bin/python"),
                         ("/usr/local/bin", "python"))

    def test_splitext(self):
        self.assertEqual(os.path.splitext("/a/b.txt"), ("/a/b", ".txt"))
        self.assertEqual(os.path.splitext("/a/b"), ("/a/b", ""))

    def test_basename_dirname(self):
        self.assertEqual(os.path.basename("/a/b/c"), "c")
        self.assertEqual(os.path.dirname("/a/b/c"), "/a/b")


unittest.main(globals())
