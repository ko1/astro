"""Stub for test.multibytecodec_support."""
import unittest


class TestBase:
    """Base for codec tests — pystro doesn't implement multibyte codecs."""
    encoding = ""
    codec_class = None
    has_iso10646 = False

    def setUp(self):
        raise unittest.SkipTest("multibyte codec not supported")


class TestBase_Mapping(TestBase):
    pass


class TestBase_Mapping_Decoder(TestBase_Mapping):
    pass


class TestBase_Mapping_Encoder(TestBase_Mapping):
    pass


def load_teststring(name):
    """Return (utf8_bytes, mapping_bytes) — both empty placeholders."""
    return (b"", b"")
