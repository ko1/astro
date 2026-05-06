import unittest


class DictMissingTest(unittest.TestCase):
    def test_missing_called(self):
        class D(dict):
            def __missing__(self, key):
                return "default:" + str(key)
        d = D(a=1)
        self.assertEqual(d["a"], 1)
        self.assertEqual(d["z"], "default:z")

    def test_get_does_not_invoke(self):
        class D(dict):
            def __missing__(self, key):
                return "default"
        d = D()
        # .get() returns the second arg without calling __missing__.
        self.assertEqual(d.get("z", "fallback"), "fallback")

    def test_in_does_not_invoke(self):
        class D(dict):
            def __missing__(self, key):
                return "default"
        d = D(a=1)
        self.assertIn("a", d)
        self.assertNotIn("z", d)


unittest.main(globals())
