# StringIO#write does not transcode into a US-ASCII/BINARY buffer, and appending
# at the end adopts a non-ASCII string's encoding into an ASCII-only buffer
# (rb_str_buf_append); $/ is a frozen US-ASCII "\n".
require 'stringio'
s = "".force_encoding("US-ASCII"); s << "あ"; p s.encoding
io = StringIO.new("".force_encoding("US-ASCII")); io << "あ"; p io.string.encoding, io.string
io = StringIO.new("".force_encoding("US-ASCII")); io.seek(0, IO::SEEK_END); io.write("あ".encode("EUC-JP")); p io.string.encoding
io = StringIO.new("ab".force_encoding("US-ASCII")); io.write("い"); p io.string.encoding, io.string.bytes
io = StringIO.new("ab"); io.write("x"); p io.string.encoding, io.string
io = StringIO.new("".b); io << "あ"; p io.string.encoding
io = StringIO.new(+"é"); io << "い"; p io.string.encoding, io.string
io = StringIO.new("abc".force_encoding("US-ASCII")); io.pos = 1; io.write("い"); p io.string.encoding
io = StringIO.new(+"", "a"); io.write("x"); io.write("ü"); p io.string.encoding, io.string
p $/.encoding, $/.frozen?, $/
