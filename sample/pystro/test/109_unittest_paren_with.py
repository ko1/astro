import unittest


class CM:
    def __init__(self, name, log):
        self.name = name; self.log = log
    def __enter__(self):
        self.log.append("enter " + self.name)
        return self
    def __exit__(self, *a):
        self.log.append("exit " + self.name)


class ParenWithTest(unittest.TestCase):
    def test_two_with_as(self):
        log = []
        with (CM("a", log) as a, CM("b", log) as b):
            log.append("body")
            self.assertEqual(a.name, "a")
            self.assertEqual(b.name, "b")
        self.assertEqual(log, ["enter a", "enter b", "body", "exit b", "exit a"])

    def test_no_as(self):
        log = []
        with (CM("a", log), CM("b", log)):
            log.append("body")
        self.assertIn("enter a", log)
        self.assertIn("enter b", log)
        self.assertIn("body", log)

    def test_trailing_comma(self):
        log = []
        with (CM("a", log) as a,):
            self.assertEqual(a.name, "a")


unittest.main(globals())
