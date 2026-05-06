import unittest


class FormatZeroPadTest(unittest.TestCase):
    def test_zero_pad_int(self):
        self.assertEqual(f"{42:05}", "00042")
        self.assertEqual(f"{42:05d}", "00042")
        self.assertEqual(f"{-42:05}", "-0042")
        self.assertEqual(f"{42:+05}", "+0042")

    def test_default_align_int(self):
        self.assertEqual(f"{42:5}", "   42")  # numeric: right-align
        self.assertEqual(f"{'x':5}", "x    ")  # string: left-align

    def test_zero_pad_with_prefix(self):
        self.assertEqual(f"{42:#06x}", "0x002a")
        self.assertEqual(f"{42:#06o}", "0o0052")
        self.assertEqual(f"{5:#06b}", "0b0101")

    def test_fill_char(self):
        self.assertEqual(f"{42:_>5}", "___42")
        self.assertEqual(f"{42:*<5}", "42***")
        self.assertEqual(f"{42:^5}", " 42  ")

    def test_thousands(self):
        self.assertEqual(f"{1234567:,}", "1,234,567")
        self.assertEqual(f"{1234567:_}", "1_234_567")

    def test_float_format(self):
        self.assertEqual(f"{3.14159:.2f}", "3.14")
        self.assertEqual(f"{3.14159:8.2f}", "    3.14")
        self.assertEqual(f"{3.14159:08.2f}", "00003.14")


class ExecKwargTest(unittest.TestCase):
    def test_exec_with_globals(self):
        # CPython's exec(code, globals_dict) — pystro accepts but ignores the
        # second arg.  Just verify it doesn't TypeError.
        exec("__pystro_exec_test__ = 42", {})

    def test_exec_three_args(self):
        exec("__pystro_exec_test2__ = 1", {}, {})


unittest.main(globals())
