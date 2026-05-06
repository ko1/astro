import unittest


class DottedImportTest(unittest.TestCase):
    def test_os_path(self):
        import os.path
        # Top-level name os is bound; os.path is reachable via attribute.
        self.assertTrue(os.path.exists("/tmp"))
        # Standalone os attributes still work.
        self.assertIsInstance(os.getcwd(), str)

    def test_os_path_as_alias(self):
        import os.path as p
        # Aliased to the leaf module.
        self.assertTrue(p.exists("/tmp"))


class FromImportTest(unittest.TestCase):
    def test_from_os(self):
        from os import getcwd
        self.assertIsInstance(getcwd(), str)

    def test_from_collections(self):
        from collections import Counter, deque
        c = Counter("aabbc")
        self.assertEqual(c["a"], 2)
        d = deque([1, 2, 3])
        self.assertEqual(d.popleft(), 1)

    def test_from_collections_as(self):
        from collections import OrderedDict as OD
        d = OD([("a", 1), ("b", 2)])
        self.assertEqual(list(d), ["a", "b"])


unittest.main(globals())
