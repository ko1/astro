import unittest
import argparse


class ArgparseExtrasTest(unittest.TestCase):
    def test_choices(self):
        p = argparse.ArgumentParser()
        p.add_argument("--mode", choices=["a", "b"])
        ns = p.parse_args(["--mode", "a"])
        self.assertEqual(ns.mode, "a")

    def test_choices_invalid(self):
        p = argparse.ArgumentParser()
        p.add_argument("--mode", choices=["a", "b"])
        with self.assertRaises(SystemExit):
            p.parse_args(["--mode", "x"])

    def test_nargs_plus(self):
        p = argparse.ArgumentParser()
        p.add_argument("nums", nargs="+", type=int)
        ns = p.parse_args(["1", "2", "3"])
        self.assertEqual(ns.nums, [1, 2, 3])

    def test_nargs_star(self):
        p = argparse.ArgumentParser()
        p.add_argument("nums", nargs="*", type=int)
        ns = p.parse_args(["1", "2"])
        self.assertEqual(ns.nums, [1, 2])

    def test_basic(self):
        p = argparse.ArgumentParser()
        p.add_argument("--foo", type=int, default=10)
        p.add_argument("--bar", default="hi")
        ns = p.parse_args(["--foo", "42"])
        self.assertEqual(ns.foo, 42)
        self.assertEqual(ns.bar, "hi")

    def test_store_true(self):
        p = argparse.ArgumentParser()
        p.add_argument("-v", "--verbose", action="store_true")
        ns = p.parse_args(["-v"])
        self.assertTrue(ns.verbose)
        ns = p.parse_args([])
        self.assertFalse(ns.verbose)

    def test_positional(self):
        p = argparse.ArgumentParser()
        p.add_argument("name")
        p.add_argument("count", type=int)
        ns = p.parse_args(["alice", "5"])
        self.assertEqual((ns.name, ns.count), ("alice", 5))


unittest.main(globals())
