# UTF-16/32 scrub per code unit, upto encoding check, invalid each_codepoint enumerator,
# rindex with \G, grapheme clusters in UTF-16 (2026-10-15)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:scrub16le) { ("abc".encode("UTF-16LE").b + "\x00".b).force_encoding("UTF-16LE").scrub("*".encode("UTF-16LE")).bytes }
t(:scrub16be) { ("ab".encode("UTF-16BE").b + "\xDC\x00\x00".b).force_encoding("UTF-16BE").scrub.bytes }
t(:scrub32le) { ("a".encode("UTF-32LE").b + "\x00\x00\x11\x00\x01".b).force_encoding("UTF-32LE").scrub.bytes }
t(:scrub16_blk) { ("a".encode("UTF-16LE").b + "\x00\xD8\x41\x00".b).force_encoding("UTF-16LE").scrub { |b| "<#{b.bytes}>".encode("UTF-16LE") }.encode("UTF-8") }
t(:upto_incompat) { "a".force_encoding("EUC-JP").upto("b".force_encoding("ISO-2022-JP")).to_a }
t(:upto_binary) { "a".upto("c".b).to_a }
t(:each_cp_invalid) { e = "\xFFa".each_codepoint; e.class }
t(:rindex_g) { ["abcabc".rindex(/\Gabc/, 3), "abcabc".rindex(/\Gb/, 4), "xyz".rindex(/\Gz/)] }
t(:graph16) { "éx".encode("UTF-16LE").grapheme_clusters.map { _1.encode("UTF-8") } }
t(:valid_gb) { ["\xB0\xA1".force_encoding("GB2312").valid_encoding?, "\xB0".force_encoding("GB2312").valid_encoding?] }
