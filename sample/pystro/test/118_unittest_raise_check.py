import unittest


class RaiseTypeCheckTest(unittest.TestCase):
    def test_int(self):
        with self.assertRaises(TypeError):
            raise 42

    def test_str(self):
        with self.assertRaises(TypeError):
            raise "boom"

    def test_class(self):
        # Class auto-instantiates.
        with self.assertRaises(ValueError):
            raise ValueError

    def test_instance(self):
        with self.assertRaises(KeyError):
            raise KeyError("k")


unittest.main(globals())
