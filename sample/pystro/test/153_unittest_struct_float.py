import unittest
import struct


class StructFloatTest(unittest.TestCase):
    def test_float_pack_be(self):
        self.assertEqual(struct.pack(">f", 1.5).hex(), "3fc00000")
        self.assertEqual(struct.pack(">f", -1.0).hex(), "bf800000")
        self.assertEqual(struct.pack(">f", 0.0).hex(), "00000000")

    def test_double_pack_be(self):
        self.assertEqual(struct.pack(">d", 1.5).hex(), "3ff8000000000000")
        self.assertEqual(struct.pack(">d", 0.0).hex(), "0000000000000000")

    def test_float_round_trip(self):
        for v in [0.0, 1.0, -1.0, 3.14, 1e10, 1e-10]:
            packed = struct.pack(">d", v)
            self.assertEqual(struct.unpack(">d", packed)[0], v)

    def test_float_lt_round_trip(self):
        for v in [0.5, -0.5, 2.5]:
            packed = struct.pack("<d", v)
            self.assertEqual(struct.unpack("<d", packed)[0], v)

    def test_int_round_trip(self):
        for v in [0, 1, -1, 1000, -1000, 0x7FFFFFFF, -0x80000000]:
            packed = struct.pack(">i", v)
            self.assertEqual(struct.unpack(">i", packed)[0], v)


class JsonExtraTest(unittest.TestCase):
    def test_default(self):
        import json
        class C:
            def __init__(self): self.x = 99
        out = json.dumps(C(), default=lambda o: {"x": o.x})
        self.assertEqual(out, '{"x": 99}')

    def test_object_hook(self):
        import json
        out = json.loads('{"a": 1}', object_hook=lambda d: ("hook", d))
        self.assertEqual(out, ("hook", {"a": 1}))


unittest.main(globals())
