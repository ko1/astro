# StringScanner#getch sliced by CHARACTER index, which walks the subject from
# the start on every call: scanning a large multibyte string was O(n^2) (crass
# tokenized emoji CSS 16x slower than ASCII).  It slices by byte now, so the
# characters it hands back — including broken sequences and the non-UTF-8
# encodings — must still be exactly CRuby's.
require 'strscan'

def walk(s)
  sc = StringScanner.new(s)
  out = []
  until sc.eos?
    c = sc.getch
    out << [c.bytes, c.encoding.name, sc.pos, sc.charpos, sc.matched, sc.pre_match&.bytesize]
  end
  out << [sc.getch, sc.eos?, sc.pos, sc.charpos]
  out
end

subjects = [
  "".dup,
  "abc".dup,
  "\u{1f600}:\u{1f600};".dup,
  "aあb\u{1f600}c".dup,
  "\xE3\x81".dup.force_encoding("UTF-8"),          # truncated sequence
  "a\xFFb".dup.force_encoding("UTF-8"),            # invalid lead byte
  "\x80\x80".dup.force_encoding("UTF-8"),          # bare continuation bytes
  "abc".dup.force_encoding("ASCII-8BIT"),
  "\xA4\xA2\xA4\xA4".dup.force_encoding("EUC-JP"),
  "\x82\xA0\x41".dup.force_encoding("Shift_JIS"),
  "abcあ".encode("UTF-16BE"),
  "abcあ".encode("UTF-32LE"),
]
subjects.each { |s| p walk(s) }

# charpos stays right whether or not it was materialised before the walk
sc = StringScanner.new("aあ\u{1f600}b")
p sc.charpos
3.times { sc.getch; p [sc.pos, sc.charpos] }
sc.reset
p [sc.pos, sc.charpos, sc.getch.bytes]
sc.pos = 4
p [sc.charpos, sc.getch.bytes, sc.rest.bytes]
