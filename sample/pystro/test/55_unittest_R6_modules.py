# R6 stdlib coverage: typing / asyncio stub / pickle / hashlib / dataclasses.

import unittest


class TypingTest(unittest.TestCase):
    def test_aliases(self):
        from typing import List, Dict, Optional
        # All aliases are no-op; subscripting returns the alias.
        x: List[int] = [1, 2, 3]
        self.assertEqual(x, [1, 2, 3])
        d: Dict[str, int] = {"a": 1}
        self.assertEqual(d, {"a": 1})

    def test_typevar(self):
        from typing import TypeVar
        T = TypeVar("T")
        self.assertEqual(T, "T")


class AsyncStubTest(unittest.TestCase):
    # pystro doesn't support async/await — only the asyncio stub APIs.
    def test_run_sync(self):
        import asyncio
        def fetch():
            asyncio.sleep(0)
            return 42
        result = asyncio.run(fetch)
        self.assertEqual(result, 42)

    def test_gather(self):
        import asyncio
        results = asyncio.gather(lambda: 1, lambda: 2, lambda: 3)
        self.assertEqual(results, [1, 2, 3])


class PickleTest(unittest.TestCase):
    def test_int(self):
        import pickle
        b = pickle.dumps(42)
        self.assertEqual(pickle.loads(b), 42)

    def test_str(self):
        import pickle
        b = pickle.dumps("hello")
        self.assertEqual(pickle.loads(b), "hello")

    def test_list(self):
        import pickle
        b = pickle.dumps([1, 2, 3])
        self.assertEqual(pickle.loads(b), [1, 2, 3])

    def test_dict(self):
        import pickle
        b = pickle.dumps({"a": 1, "b": [2, 3]})
        self.assertEqual(pickle.loads(b), {"a": 1, "b": [2, 3]})

    def test_nested(self):
        import pickle
        data = {"name": "alice", "tags": ["x", "y"], "nested": {"k": 1}}
        self.assertEqual(pickle.loads(pickle.dumps(data)), data)

    def test_bool_none(self):
        import pickle
        self.assertEqual(pickle.loads(pickle.dumps(True)), True)
        self.assertEqual(pickle.loads(pickle.dumps(None)), None)


class HashlibTest(unittest.TestCase):
    def test_md5(self):
        import hashlib
        # Known vectors.
        self.assertEqual(hashlib.md5(b"").hexdigest(),
                         "d41d8cd98f00b204e9800998ecf8427e")
        self.assertEqual(hashlib.md5(b"abc").hexdigest(),
                         "900150983cd24fb0d6963f7d28e17f72")
        self.assertEqual(hashlib.md5(b"The quick brown fox jumps over the lazy dog").hexdigest(),
                         "9e107d9d372bb6826bd81d3542a419d6")

    def test_sha256(self):
        import hashlib
        self.assertEqual(hashlib.sha256(b"").hexdigest(),
                         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        self.assertEqual(hashlib.sha256(b"abc").hexdigest(),
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")

    def test_str_input(self):
        import hashlib
        self.assertEqual(hashlib.md5("abc").hexdigest(),
                         hashlib.md5(b"abc").hexdigest())


class DataclassTest(unittest.TestCase):
    def test_basic(self):
        from dataclasses import dataclass, asdict, astuple

        @dataclass
        class P:
            x = 0
            y = 0
            _fields = ("x", "y")

        p = P(3, 4)
        self.assertEqual(p.x, 3)
        self.assertEqual(p.y, 4)
        self.assertEqual(asdict(p), {"x": 3, "y": 4})
        self.assertEqual(astuple(p), (3, 4))
        self.assertEqual(P(1, 2), P(1, 2))
        self.assertNotEqual(P(1, 2), P(1, 3))

    def test_make(self):
        from dataclasses import make_dataclass
        Q = make_dataclass("Q", ["a", "b", "c"])
        q = Q(1, 2, 3)
        self.assertEqual(q.a, 1)
        self.assertEqual(q.b, 2)
        self.assertEqual(q.c, 3)


class ArgparseTest(unittest.TestCase):
    def test_basic(self):
        from argparse import ArgumentParser
        p = ArgumentParser()
        p.add_argument("--name")
        p.add_argument("--count", type=int)
        p.add_argument("input")
        ns = p.parse_args(["--name=alice", "--count=5", "in.txt"])
        self.assertEqual(ns.name, "alice")
        self.assertEqual(ns.count, 5)
        self.assertEqual(ns.input, "in.txt")

    def test_flag(self):
        from argparse import ArgumentParser
        p = ArgumentParser()
        p.add_argument("-v", "--verbose", action="store_true")
        ns = p.parse_args(["-v"])
        self.assertEqual(ns.verbose, True)
        ns = p.parse_args([])
        self.assertEqual(ns.verbose, False)


class EnumTest(unittest.TestCase):
    def test_basic(self):
        from enum import _make_enum
        Color = _make_enum("Color", {"RED": 1, "GREEN": 2})
        self.assertEqual(Color.RED.name, "RED")
        self.assertEqual(Color.RED.value, 1)
        self.assertEqual(Color.RED, Color.RED)
        self.assertNotEqual(Color.RED, Color.GREEN)


class CollectionsTest(unittest.TestCase):
    def test_deque(self):
        from collections import deque
        d = deque([1, 2, 3])
        d.append(4)
        d.appendleft(0)
        self.assertEqual(list(d), [0, 1, 2, 3, 4])
        self.assertEqual(d.popleft(), 0)
        self.assertEqual(d.pop(), 4)

    def test_counter(self):
        from collections import Counter
        c = Counter("abracadabra")
        self.assertEqual(c["a"], 5)
        self.assertEqual(c["b"], 2)
        self.assertEqual(c["z"], 0)
        top = c.most_common(2)
        self.assertEqual(top[0], ("a", 5))

    def test_defaultdict(self):
        from collections import defaultdict
        dd = defaultdict(list)
        dd["x"].append(1)
        dd["x"].append(2)
        self.assertEqual(dd["x"], [1, 2])


class FunctoolsTest(unittest.TestCase):
    def test_reduce(self):
        from functools import reduce
        self.assertEqual(reduce(lambda a, b: a + b, [1, 2, 3]), 6)
        self.assertEqual(reduce(lambda a, b: a * b, [1, 2, 3, 4]), 24)

    def test_partial(self):
        from functools import partial
        from operator import mul
        triple = partial(mul, 3)
        self.assertEqual(triple(7), 21)

    def test_cache(self):
        from functools import cache
        @cache
        def fib(n):
            if n < 2: return n
            return fib(n-1) + fib(n-2)
        self.assertEqual(fib(20), 6765)


unittest.main(globals())
