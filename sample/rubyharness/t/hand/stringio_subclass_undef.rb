# StringIO's own reads / checks do not dispatch through methods a subclass undefines
# or aliases (fakefs undefs closed_write? and aliases sysread to read); an ASCII-
# incompatible encoding needs binmode for reading; encoding: takes an Encoding
require "stringio"
class F < StringIO
  undef_method :closed_read?, :closed_write?, :size, :length, :string
  alias sysread read
end
f = F.new(+"abc", "r+")
f.write("x"); f.rewind
p f.read(2), f.readpartial(5), (f.readpartial(1) rescue $!.class)
require "stringio"
[["r:UTF-32LE", {}], ["rb:UTF-32LE", {}], ["w:UTF-32LE", {}], ["r+:UTF-16BE", {}], ["r", {encoding: Encoding::UTF_32LE}], ["r", {binmode: true, encoding: Encoding::UTF_32LE}],
 ["w", {encoding: Encoding::UTF_32LE}], ["r", {encoding: "UTF-8"}], ["r:UTF-32LE", {binmode: true}], ["a:UTF-16LE", {}]].each do |m, kw|
  r = begin; s = StringIO.new(+"hi", m, **kw); [s.external_encoding, s.string.encoding]; rescue => e; [e.class, e.message]; end
  p [m, kw, r[0]]
end
