import unittest
from collections import OrderedDict


class OrderedDictEqTest(unittest.TestCase):
    def test_same_order_eq(self):
        self.assertEqual(OrderedDict([("a", 1), ("b", 2)]),
                         OrderedDict([("a", 1), ("b", 2)]))

    def test_different_order_ne(self):
        self.assertNotEqual(OrderedDict([("a", 1), ("b", 2)]),
                            OrderedDict([("b", 2), ("a", 1)]))

    def test_vs_plain_dict_order_insensitive(self):
        # OrderedDict == dict: ignores order (CPython behaviour).
        od = OrderedDict([("a", 1), ("b", 2)])
        self.assertEqual(od, {"b": 2, "a": 1})
        self.assertEqual(od, {"a": 1, "b": 2})

    def test_value_diff(self):
        self.assertNotEqual(OrderedDict([("a", 1)]),
                            OrderedDict([("a", 2)]))


unittest.main(globals())
