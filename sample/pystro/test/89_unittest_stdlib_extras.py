import unittest


class CounterExtrasTest(unittest.TestCase):
    def test_subtract(self):
        from collections import Counter
        c = Counter([1, 1, 2, 2, 3])
        c.subtract([1, 2])
        self.assertEqual(c[1], 1)
        self.assertEqual(c[2], 1)
        self.assertEqual(c[3], 1)

    def test_subtract_dict(self):
        from collections import Counter
        c = Counter("aaab")
        c.subtract({"a": 2})
        self.assertEqual(c["a"], 1)
        self.assertEqual(c["b"], 1)

    def test_total(self):
        from collections import Counter
        c = Counter([1, 1, 2])
        self.assertEqual(c.total(), 3)

    def test_elements(self):
        from collections import Counter
        c = Counter("ab")
        c["a"] += 1
        # Counter("ab") gives a=1, b=1; +=1 makes a=2.
        self.assertEqual(sorted(list(c.elements())), ["a", "a", "b"])


class DequeExtrasTest(unittest.TestCase):
    def test_maxlen(self):
        from collections import deque
        d = deque([1, 2, 3], maxlen=3)
        d.append(4)
        self.assertEqual(list(d), [2, 3, 4])

    def test_rotate(self):
        from collections import deque
        d = deque([1, 2, 3, 4, 5])
        d.rotate(2)
        self.assertEqual(list(d), [4, 5, 1, 2, 3])

    def test_extend(self):
        from collections import deque
        d = deque([1, 2])
        d.extend([3, 4])
        self.assertEqual(list(d), [1, 2, 3, 4])
        d.extendleft([0, -1])
        self.assertEqual(list(d), [-1, 0, 1, 2, 3, 4])


class ProductRepeatTest(unittest.TestCase):
    def test_repeat(self):
        import itertools
        out = list(itertools.product([0, 1], repeat=2))
        self.assertEqual(out, [(0, 0), (0, 1), (1, 0), (1, 1)])

    def test_repeat3(self):
        import itertools
        out = list(itertools.product([0, 1], repeat=3))
        self.assertEqual(len(out), 8)


class IntEnumTest(unittest.TestCase):
    def test_basic(self):
        from enum import IntEnum
        class C(IntEnum):
            OK = 200
            BAD = 400
        self.assertEqual(C.OK, 200)
        self.assertEqual(C.BAD.value, 400)


unittest.main(globals())
