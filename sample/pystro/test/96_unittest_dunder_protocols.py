import unittest
import math


class GetitemIterTest(unittest.TestCase):
    def test_seq_protocol(self):
        class S:
            def __init__(self, lst): self.lst = lst
            def __getitem__(self, i):
                if i >= len(self.lst): raise IndexError
                return self.lst[i]
        self.assertEqual(list(S([10, 20, 30])), [10, 20, 30])

    def test_in_via_getitem(self):
        class S:
            def __init__(self, lst): self.lst = lst
            def __getitem__(self, i):
                if i >= len(self.lst): raise IndexError
                return self.lst[i]
        s = S(["a", "b", "c"])
        self.assertIn("b", s)
        self.assertNotIn("z", s)


class IndexProtocolTest(unittest.TestCase):
    def test_index(self):
        class N:
            def __init__(self, v): self.v = v
            def __index__(self): return self.v
        self.assertEqual([10, 20, 30, 40][N(2)], 30)


class RoundFloorCeilTest(unittest.TestCase):
    def test_round(self):
        log = []
        class M:
            def __round__(self, n=0):
                log.append(("round", n))
                return 42
        self.assertEqual(round(M()), 42)
        self.assertEqual(round(M(), 3), 42)
        self.assertEqual(log, [("round", 0), ("round", 3)])

    def test_floor_ceil(self):
        class M:
            def __floor__(self): return 1
            def __ceil__(self): return 99
        self.assertEqual(math.floor(M()), 1)
        self.assertEqual(math.ceil(M()), 99)


class IterContainsViaGenTest(unittest.TestCase):
    def test_iter_returns_generator(self):
        class C:
            def __iter__(self):
                yield from [1, 2, 3]
        self.assertIn(2, C())
        self.assertNotIn(99, C())
        self.assertEqual(list(C()), [1, 2, 3])


unittest.main(globals())
