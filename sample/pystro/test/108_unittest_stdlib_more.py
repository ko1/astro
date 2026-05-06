import unittest


class UserDictTest(unittest.TestCase):
    def test_subclass(self):
        from collections import UserDict
        class CD(UserDict):
            def __setitem__(self, k, v):
                super().__setitem__(k.lower(), v)
            def __getitem__(self, k):
                return super().__getitem__(k.lower())
        d = CD()
        d["FOO"] = 1
        d["Bar"] = 2
        self.assertEqual(d["foo"], 1)
        self.assertEqual(d["bar"], 2)


class TemplateTest(unittest.TestCase):
    def test_substitute(self):
        from string import Template
        t = Template("Hello, $name!")
        self.assertEqual(t.substitute(name="World"), "Hello, World!")

    def test_braced(self):
        from string import Template
        t = Template("${greeting}-${name}")
        self.assertEqual(t.substitute(greeting="hi", name="alice"),
                         "hi-alice")

    def test_safe(self):
        from string import Template
        t = Template("$missing")
        self.assertEqual(t.safe_substitute({}), "$missing")

    def test_strict_missing(self):
        from string import Template
        t = Template("$x")
        with self.assertRaises(KeyError):
            t.substitute({})


class BisectKeyTest(unittest.TestCase):
    def test_bisect_with_key(self):
        import bisect
        xs = [(1, "a"), (3, "b"), (5, "c")]
        i = bisect.bisect_left(xs, 3, key=lambda x: x[0])
        self.assertEqual(i, 1)


unittest.main(globals())
