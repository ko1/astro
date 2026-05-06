import unittest
import sys


class SysExitTest(unittest.TestCase):
    def test_no_arg(self):
        try:
            sys.exit()
        except SystemExit as e:
            self.assertIsNone(e.code)

    def test_int_arg(self):
        try:
            sys.exit(42)
        except SystemExit as e:
            self.assertEqual(e.code, 42)

    def test_str_arg(self):
        try:
            sys.exit("error")
        except SystemExit as e:
            self.assertEqual(e.code, "error")

    def test_caught_normally(self):
        # SystemExit must be catchable as a regular exception.
        called = False
        try:
            sys.exit(1)
        except SystemExit:
            called = True
        self.assertTrue(called)


class SysExcInfoTest(unittest.TestCase):
    def test_inside_except(self):
        try:
            raise ValueError("x")
        except ValueError:
            info = sys.exc_info()
            self.assertIs(info[0], ValueError)
            self.assertEqual(str(info[1]), "x")

    def test_outside_except(self):
        info = sys.exc_info()
        self.assertEqual(info, (None, None, None))


unittest.main(globals())
