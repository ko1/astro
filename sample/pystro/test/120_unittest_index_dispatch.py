import unittest


class IndexDispatchTest(unittest.TestCase):
    def test_hex_oct_bin(self):
        class N:
            def __index__(self): return 255
        self.assertEqual(hex(N()), "0xff")
        self.assertEqual(oct(N()), "0o377")
        self.assertEqual(bin(N()), "0b11111111")

    def test_hex_negative(self):
        class N:
            def __index__(self): return -10
        self.assertEqual(hex(N()), "-0xa")


unittest.main(globals())
