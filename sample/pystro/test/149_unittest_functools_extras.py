import unittest
import functools


class WrapsTest(unittest.TestCase):
    def test_copies_name_and_doc(self):
        def src():
            "src docstring"
        @functools.wraps(src)
        def dst(): pass
        self.assertEqual(dst.__name__, "src")
        self.assertEqual(dst.__doc__, "src docstring")
        self.assertIs(dst.__wrapped__, src)


class LRUCacheTest(unittest.TestCase):
    def test_caches(self):
        @functools.lru_cache(maxsize=2)
        def f(n): return n * 2
        self.assertEqual(f(1), 2)
        self.assertEqual(f(2), 4)
        self.assertEqual(f(1), 2)
        info = f.cache_info()
        self.assertEqual(info.hits, 1)
        self.assertEqual(info.misses, 2)
        self.assertEqual(info.currsize, 2)

    def test_eviction(self):
        @functools.lru_cache(maxsize=2)
        def f(n): return n
        f(1); f(2); f(3)
        # 1 should be evicted by now.
        info = f.cache_info()
        self.assertEqual(info.currsize, 2)

    def test_clear(self):
        @functools.lru_cache(maxsize=10)
        def f(n): return n
        f(1); f(2)
        f.cache_clear()
        self.assertEqual(f.cache_info().currsize, 0)
        self.assertEqual(f.cache_info().hits, 0)


class FStringConcatTest(unittest.TestCase):
    def test_fstr_fstr(self):
        self.assertEqual(f"a{1}" f"b{2}", "a1b2")

    def test_fstr_str(self):
        self.assertEqual(f"x={1}" "literal", "x=1literal")

    def test_str_fstr(self):
        self.assertEqual("plain " f"{2}" " end", "plain 2 end")

    def test_paren_split(self):
        x = (f"hi {1}, "
             f"bye {2}")
        self.assertEqual(x, "hi 1, bye 2")


class CounterReprTest(unittest.TestCase):
    def test_descending(self):
        import collections
        # CPython orders by descending count.
        self.assertEqual(repr(collections.Counter("aabbbc")),
                         "Counter({'b': 3, 'a': 2, 'c': 1})")


unittest.main(globals())
