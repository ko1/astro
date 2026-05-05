import unittest
import copy


class DeepCopyTest(unittest.TestCase):
    def test_user_class_isolated(self):
        class Box:
            def __init__(self, v): self.v = v
        b = Box([1, 2, 3])
        d = copy.deepcopy(b)
        b.v.append(4)
        self.assertEqual(d.v, [1, 2, 3])

    def test_copy_hook(self):
        class WithHook:
            def __init__(self, x): self.x = x
            def __copy__(self):
                return WithHook(self.x + "-copy")
        c = copy.copy(WithHook("orig"))
        self.assertEqual(c.x, "orig-copy")

    def test_deepcopy_hook(self):
        log = []
        class WithHook:
            def __init__(self, x): self.x = x
            def __deepcopy__(self, memo):
                log.append("hook")
                return WithHook(self.x * 2)
        d = copy.deepcopy(WithHook("a"))
        self.assertEqual(d.x, "aa")
        self.assertEqual(log, ["hook"])


class StatisticsTest(unittest.TestCase):
    def test_mean(self):
        import statistics
        self.assertEqual(statistics.mean([1, 2, 3, 4, 5]), 3.0)

    def test_median(self):
        import statistics
        self.assertEqual(statistics.median([1, 3, 5, 7, 9]), 5)
        self.assertEqual(statistics.median([1, 2, 3, 4]), 2.5)

    def test_mode(self):
        import statistics
        self.assertEqual(statistics.mode([1, 1, 2, 3]), 1)


class WeakrefStubTest(unittest.TestCase):
    def test_basic(self):
        import weakref
        class O: pass
        o = O()
        w = weakref.ref(o)
        self.assertIs(w(), o)


unittest.main(globals())
