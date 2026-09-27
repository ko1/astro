module Meths; end
module MethsMore; end
class UM
  def marshal_dump = :data
  def marshal_load(d) = p([:marshal_load, is_a?(Meths), d])
end

# 'e' extends before marshal_load, and rejects a missing / non-module constant
Marshal.load("\x04\be:\nMethsU:\x07UM:\tdata")
p((Marshal.load("\x04\be:\x16NonExistentModuleo:\vObject\x00") rescue $!.class))
p((Marshal.load("\x04\be:\vStringo:\vObject\x00") rescue $!.class))
o = Marshal.load(Marshal.dump(Object.new.extend(Meths, MethsMore)))
p o.singleton_class.ancestors.drop(1).take(2)
p Object.new.extend(Meths, MethsMore).singleton_class.ancestors.drop(1).take(2)

# a loaded Regexp is frozen: extend / ivars raise
p((Marshal.load("\004\be:\nMethse:\016MethsMore/\n[a-z]\000") rescue $!.class))
p((Marshal.load("\x04\bI/\nhello\x00\a:\x06EF:\x11@regexp_ivar[\x06i/") rescue $!.class))

# encoded Symbols, and aliases that share one encoding
p Marshal.load("\x04\bI:\a\x81\xA8\x06:\rencoding\"\x10Windows-31J") == "\u2192".encode("sjis").to_sym
p Marshal.load("\x04\bI:\t\xFE\xFF!\x92\x06:\rencoding\"\vUTF-16").encoding
p Marshal.load("\x04\b:\b\xE2\x86\x92").encoding
p "\x81\xA8".b.force_encoding("CP932") == "\u2192".encode("SJIS")
p "a".force_encoding("646").encoding, "a".force_encoding("CP65001").encoding
