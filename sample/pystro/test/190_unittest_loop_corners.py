import unittest


class LoopCornerTest(unittest.TestCase):
    def test_for_else_raise(self):
        def f():
            try:
                for i in range(3):
                    if i == 1: raise ValueError
                else:
                    return "else"
            except ValueError:
                return "exc"
        self.assertEqual(f(), "exc")

    def test_for_else_no_break(self):
        def f():
            for i in range(3):
                if i == 99: break
            else:
                return "no break"
            return "had break"
        self.assertEqual(f(), "no break")

    def test_for_else_with_break(self):
        def f():
            for i in range(3):
                if i == 1: break
            else:
                return "no break"
            return "had break"
        self.assertEqual(f(), "had break")

    def test_continue_in_finally(self):
        def f():
            results = []
            for i in range(3):
                try:
                    if i == 1: raise ValueError
                except ValueError:
                    continue
                finally:
                    results.append(("fin", i))
                results.append(("body", i))
            return results
        self.assertEqual(f(),
                         [("fin", 0), ("body", 0),
                          ("fin", 1),
                          ("fin", 2), ("body", 2)])

    def test_try_in_for(self):
        def f():
            results = []
            for i in range(5):
                try:
                    if i == 2: raise ValueError
                    results.append(i)
                except ValueError:
                    results.append("exc")
            return results
        self.assertEqual(f(), [0, 1, "exc", 3, 4])


class DictConstructorTest(unittest.TestCase):
    def test_fromkeys(self):
        self.assertEqual(dict.fromkeys(range(3), "x"),
                         {0: "x", 1: "x", 2: "x"})

    def test_dict_from_pairs(self):
        self.assertEqual(dict([("a", 1), ("b", 2)]), {"a": 1, "b": 2})

    def test_dict_kwargs(self):
        self.assertEqual(dict(a=1, b=2), {"a": 1, "b": 2})

    def test_dict_mixed(self):
        self.assertEqual(dict([("a", 1)], b=2), {"a": 1, "b": 2})


unittest.main(globals())
