"""pystro stub for `email.charset`."""


QP = 1
BASE64 = 2
SHORTEST = 3


class Charset:
    def __init__(self, input_charset="us-ascii"):
        self.input_charset = input_charset
        self.output_charset = input_charset
        self.input_codec = "ascii"
        self.output_codec = "ascii"
        self.header_encoding = SHORTEST
        self.body_encoding = SHORTEST
    def __str__(self): return self.input_charset
    def __eq__(self, o): return isinstance(o, Charset) and self.input_charset == o.input_charset
    def __ne__(self, o): return not self.__eq__(o)
    def get_body_encoding(self): return None
    def get_output_charset(self): return self.output_charset
    def header_encode(self, s): return s
    def header_encode_lines(self, s, *a): return [s]
    def body_encode(self, s): return s


def add_charset(charset, header_enc=None, body_enc=None, output_charset=None):
    pass


def add_alias(alias, canonical):
    pass


def add_codec(charset, codecname):
    pass


__all__ = ["Charset", "add_charset", "add_alias", "add_codec",
           "QP", "BASE64", "SHORTEST"]
