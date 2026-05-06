"""Stub for test.multibytecodec_support."""
import unittest


class TestBase:
    """Base for codec tests — pystro doesn't implement multibyte codecs."""
    encoding = ""
    codec_class = None
    has_iso10646 = False

    def setUp(self):
        raise unittest.SkipTest("multibyte codec not supported")
