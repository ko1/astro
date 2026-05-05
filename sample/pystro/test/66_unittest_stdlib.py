import unittest


class DatetimeTest(unittest.TestCase):
    def test_date(self):
        import datetime
        d = datetime.date(2024, 1, 15)
        self.assertEqual(d.year, 2024)
        self.assertEqual(d.month, 1)
        self.assertEqual(d.day, 15)
        self.assertEqual(d.isoformat(), "2024-01-15")

    def test_datetime(self):
        import datetime
        dt = datetime.datetime(2024, 1, 15, 10, 30, 45)
        self.assertEqual(dt.isoformat(), "2024-01-15T10:30:45")
        self.assertEqual(dt.isoformat(" "), "2024-01-15 10:30:45")

    def test_timedelta(self):
        import datetime
        td = datetime.timedelta(days=1, hours=2)
        self.assertEqual(td.total_seconds(), 86400.0 + 7200.0)
        self.assertEqual(td.days, 1)


class Base64Test(unittest.TestCase):
    def test_roundtrip(self):
        import base64
        for data in [b"", b"a", b"ab", b"abc", b"abcd", b"Hello, World!"]:
            encoded = base64.b64encode(data)
            decoded = base64.b64decode(encoded)
            self.assertEqual(decoded, data)

    def test_known_vector(self):
        import base64
        self.assertEqual(base64.b64encode(b"Hello!"), b"SGVsbG8h")
        self.assertEqual(base64.b64decode(b"SGVsbG8h"), b"Hello!")


class BinasciiTest(unittest.TestCase):
    def test_hexlify(self):
        import binascii
        self.assertEqual(binascii.hexlify(b"\x01\x02\x03"), b"010203")
        self.assertEqual(binascii.unhexlify("010203"), b"\x01\x02\x03")

    def test_crc32(self):
        import binascii
        # Known CRC32 of "Hello, World!" = 0xEC4AC3D0
        self.assertEqual(binascii.crc32(b"Hello, World!"), 0xEC4AC3D0)


class StructTest(unittest.TestCase):
    def test_calcsize(self):
        import struct
        self.assertEqual(struct.calcsize(">II"), 8)
        self.assertEqual(struct.calcsize(">H"), 2)

    def test_pack_unpack(self):
        import struct
        self.assertEqual(struct.pack(">H", 256), b"\x01\x00")
        self.assertEqual(struct.unpack(">H", b"\x01\x00"), (256,))


class ContextlibTest(unittest.TestCase):
    def test_contextmanager(self):
        import contextlib
        log = []
        @contextlib.contextmanager
        def cm():
            log.append("enter")
            yield 42
            log.append("exit")
        with cm() as x:
            log.append("body:" + str(x))
        self.assertEqual(log, ["enter", "body:42", "exit"])

    def test_suppress(self):
        import contextlib
        with contextlib.suppress(ValueError):
            raise ValueError("ignored")
        # No exception propagates.


class ReTest(unittest.TestCase):
    def test_findall(self):
        import re
        self.assertEqual(re.findall(r"\d+", "1 a 23 b 456"), ["1", "23", "456"])

    def test_search(self):
        import re
        m = re.search(r"hello", "say hello world")
        self.assertEqual(m.group(), "hello")

    def test_match(self):
        import re
        m = re.match(r"\d+", "123abc")
        self.assertEqual(m.group(), "123")

    def test_sub(self):
        import re
        self.assertEqual(re.sub(r"\d+", "X", "a1b22c333"), "aXbXcX")

    def test_split(self):
        import re
        self.assertEqual(re.split(r"\s+", "a b  c"), ["a", "b", "c"])


class RawStringTest(unittest.TestCase):
    def test_raw(self):
        self.assertEqual(r"hello\n", "hello\\n")
        self.assertEqual(len(r"\d+"), 3)
        self.assertEqual(rb"\x00", b"\\x00")


class MatchSoftKeywordTest(unittest.TestCase):
    def test_match_as_var(self):
        match = "hello"
        self.assertEqual(match, "hello")

    def test_match_method(self):
        class C:
            def match(self, s):
                return s + "!"
        self.assertEqual(C().match("yo"), "yo!")


unittest.main(globals())
