# StringScanner re-validates the subject's encoding after reset / terminate / string=
# (a same-length setbyte included); << appends to the caller's own String.
require 'strscan'
s = +"a"; ss = StringScanner.new(s); ss.scan(/./); s.setbyte(0, 255); ss.reset; p [(ss.scan(/./) rescue $!.class)]
s2 = +"\xff".b.force_encoding("UTF-8"); ss = StringScanner.new(s2); p [(ss.scan(/./) rescue $!.class)]; s2.setbyte(0, 97); ss.reset; p ss.scan(/./)
ss = StringScanner.new(+"ab"); ss.scan(/a/); ss.string = "\xff".dup.force_encoding("UTF-8"); p [(ss.scan(/./) rescue $!.class)]
s = +"ab"; ss = StringScanner.new(s); ss << "c"; p s, ss.string.equal?(s), ss.scan(/abc/)
f = "x".freeze; ss2 = StringScanner.new(f); p [(ss2 << "y" rescue $!.class)]
