# pystro stdlib `string` constants.

ascii_lowercase = "abcdefghijklmnopqrstuvwxyz"
ascii_uppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ascii_letters   = ascii_lowercase + ascii_uppercase
digits          = "0123456789"
hexdigits       = digits + "abcdef" + "ABCDEF"
octdigits       = "01234567"
punctuation     = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
whitespace      = " \t\n\r\v\f"
printable       = digits + ascii_letters + punctuation + whitespace

__all__ = ["ascii_lowercase", "ascii_uppercase", "ascii_letters",
           "digits", "hexdigits", "octdigits",
           "punctuation", "whitespace", "printable"]
