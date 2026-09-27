# StringIO#gets(sep) searches the separator byte by byte (CRuby strio_getline):
# a UTF-16 stream read from an odd offset (rexml keeps the BOM's spare byte
# aside) still cuts right after the separator bytes.  String#index / byteindex
# only report hits that start on a character boundary.
require "stringio"
src = "﻿<a>\n<b/>".encode("UTF-16LE")
io = StringIO.new(src.b)
io.read(3)
io.set_encoding("UTF-16LE")
line = io.readline(">".encode("UTF-16LE"))
p line.bytes, line.encoding, (io.read(1).bytes rescue $!.class)
p "\x00a\x00>\x00\n".dup.force_encoding("UTF-16LE").index(">".encode("UTF-16LE")),
  "\x00a\x00>\x00\n".dup.force_encoding("UTF-16LE").byteindex(">".encode("UTF-16LE"))
p "a>b>".encode("UTF-16LE").index(">".encode("UTF-16LE")), ("a>b>".encode("UTF-16LE").byteindex(">".encode("UTF-16LE"), 3) rescue $!.class), "a>b>".encode("UTF-16LE").byteindex(">".encode("UTF-16LE"), 4)
s = StringIO.new("aXYbXYc")
p s.gets("XY"), s.gets("XY", chomp: true), s.gets("XY", chomp: true), s.gets("XY")
s = StringIO.new("héllo--wörld--")
p s.gets("--"), s.gets("--", 3), s.gets("--", chomp: true), s.gets("--"), s.lineno
p StringIO.new("abc").gets("zz"), StringIO.new("abczz").gets("zz", chomp: true), StringIO.new("x\ny").each_line("\n").to_a
p "日本語".index("語"), "日本語".byteindex("語"), "aあa".index("a", 1), "aあa".byteindex("a", 1)
require "stringio"; p StringIO.new("héllo\n\nx").gets("", 3), StringIO.new("héllo\n\nx").gets("", 2), StringIO.new("héllo\n\nx").gets("", 1)
s = StringIO.new("x"); s.set_encoding("ISO-8859-1"); p s.gets(nil).encoding
s = StringIO.new("abc"); s.gets(nil); p s.gets(nil), s.eof?, s.lineno
p StringIO.new("").gets(nil), StringIO.new("a\n\nb").gets(nil, 0) , StringIO.new("héllo\n").gets("", 2)
