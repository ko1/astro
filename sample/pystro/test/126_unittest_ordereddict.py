import unittest
from collections import OrderedDict


class OrderedDictTest(unittest.TestCase):
    def test_basic_order(self):
        od = OrderedDict([("c", 3), ("a", 1), ("b", 2)])
        self.assertEqual(list(od), ["c", "a", "b"])

    def test_move_to_end(self):
        od = OrderedDict([("a", 1), ("b", 2), ("c", 3)])
        od.move_to_end("a")
        self.assertEqual(list(od), ["b", "c", "a"])

    def test_move_to_start(self):
        od = OrderedDict([("a", 1), ("b", 2), ("c", 3)])
        od.move_to_end("c", last=False)
        self.assertEqual(list(od), ["c", "a", "b"])

    def test_popitem_last(self):
        od = OrderedDict([("a", 1), ("b", 2)])
        self.assertEqual(od.popitem(), ("b", 2))

    def test_popitem_first(self):
        od = OrderedDict([("a", 1), ("b", 2)])
        self.assertEqual(od.popitem(last=False), ("a", 1))


class ModuleNameTest(unittest.TestCase):
    def test_module_name(self):
        import json
        self.assertEqual(json.__name__, "json")
        import math
        self.assertEqual(math.__name__, "math")


unittest.main(globals())
