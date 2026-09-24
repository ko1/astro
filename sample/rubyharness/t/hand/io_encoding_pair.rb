# An ext→int encoding pair TRANSCODES what is read; it does not merely tag the
# bytes.  koruby honoured the external half everywhere but only transcoded in
# #read / #gets / #getc — #readlines, #each_line and the File.read class method
# handed back the raw bytes with a UTF-8 tag (simplecov reads source files by
# their magic comment and got replacement characters).
require "tmpdir"

def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

DIR = Dir.mktmpdir
P = File.join(DIR, "euc.txt")
File.binwrite(P, "# encoding: EUC-JP\n# \xB0\xA1\n")

t(:binread)      { File.binread(P).bytes.last(4) }
t(:read_pair)    { File.read(P, encoding: "EUC-JP:UTF-8").lines.last }
t(:read_ext)     { s = File.read(P, encoding: "EUC-JP"); [s.encoding.to_s, s.valid_encoding?] }
t(:read_opts)    { File.read(P, external_encoding: "EUC-JP", internal_encoding: "UTF-8").lines.last }
t(:readlines_cm) { File.readlines(P, encoding: "EUC-JP:UTF-8").last }
t(:foreach_cm)   { r = nil; File.foreach(P, encoding: "EUC-JP:UTF-8") { |x| r = x }; r }
t(:io_read)      { IO.read(P, encoding: "EUC-JP:UTF-8").lines.last }

t(:open_read)    { File.open(P, "r:EUC-JP:UTF-8", &:read).lines.last }
t(:open_lines)   { File.open(P, "r:EUC-JP:UTF-8", &:readlines).last }
t(:open_each)    { r = nil; File.open(P, "r:EUC-JP:UTF-8") { |f| f.each_line { |x| r = x } }; r }
t(:open_gets)    { File.open(P, "r:EUC-JP:UTF-8") { |f| f.gets; f.gets } }
t(:set_encoding) { File.open(P) { |f| f.set_encoding("EUC-JP", "UTF-8"); f.readlines.last } }
t(:encs)         { File.open(P, "r:EUC-JP:UTF-8") { |f| [f.external_encoding.to_s, f.internal_encoding.to_s] } }

# no pair → the bytes keep the external encoding, untouched
t(:ext_only)     { s = File.open(P, "r:EUC-JP", &:readlines).last; [s.encoding.to_s, s.bytes.last(3)] }
t(:no_enc)       { File.readlines(P).last.encoding.to_s }
t(:same_pair)    { File.read(P, encoding: "UTF-8:UTF-8").lines.first }

# a pair that needs no conversion still tags correctly, and Shift_JIS works too
Q = File.join(DIR, "sjis.txt")
File.binwrite(Q, "\x82\xA0\n")
t(:sjis_pair)    { File.read(Q, encoding: "Shift_JIS:UTF-8").chomp }
t(:sjis_lines)   { File.readlines(Q, encoding: "Shift_JIS:UTF-8").first.chomp }

require "fileutils"
FileUtils.remove_entry(DIR)
