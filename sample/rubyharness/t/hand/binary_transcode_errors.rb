# ASCII-8BIT is not Latin-1: CRuby has no converter either way for the high half,
# and an error message names the LEG that failed (a conversion that pivots
# through UTF-8 says so).  The binary encoding also reads as "BINARY (ASCII-8BIT)"
# inside an error message, though its #name is still "ASCII-8BIT".
p [Encoding::BINARY.name, Encoding::BINARY.to_s, Encoding::BINARY.inspect]

[
  ['BINARY -> UTF-8',      -> { "\xAE".b.encode("UTF-8") }],
  ['BINARY -> US-ASCII',   -> { "\xAE".b.encode("US-ASCII") }],
  ['BINARY -> EUC-JP',     -> { "\xAE".b.encode("EUC-JP") }],
  ['BINARY -> BINARY',     -> { "\xAE".b.encode("ASCII-8BIT") }],
  ['ascii BINARY -> UTF-8',-> { "abc".b.encode("UTF-8") }],
  ['UTF-8 -> BINARY',      -> { "é".encode("ASCII-8BIT") }],
  ['UTF-8 ascii -> BINARY',-> { "abc".encode("ASCII-8BIT") }],
  ['latin1 -> BINARY',     -> { "\xAE".dup.force_encoding("ISO-8859-1").encode("ASCII-8BIT") }],
  ['latin1 -> EUC-JP',     -> { "\xa0 hi".dup.force_encoding("ISO-8859-1").encode("EUC-JP") }],
  ['BINARY -> UTF-8 repl', -> { "a\xAEb".b.encode("UTF-8", undef: :replace, replace: "?") }],
  ['UTF-8 -> BINARY repl', -> { "aéb".encode("ASCII-8BIT", undef: :replace, replace: "?") }],
].each do |name, f|
  begin
    v = f.call
    puts "#{name} -> #{v.inspect} #{v.encoding}"
  rescue => e
    puts "#{name} -> #{e.class}: #{e.message}"
  end
end

b = "\xAE".b
begin; "é".encode("UTF-8") + b; rescue => e; p e.message; end
begin; b + "é"; rescue => e; p e.message; end
begin; ["é", b].join; rescue => e; p e.message; end
begin; "é" + "\xa4\xa2".dup.force_encoding("EUC-JP"); rescue => e; p e.message; end

# the error object carries the leg's encodings and the offending char
begin
  b.encode("UTF-8")
rescue Encoding::UndefinedConversionError => e
  p [e.source_encoding_name, e.destination_encoding_name, e.error_char]
end

# String#split checks the separator's encoding against the receiver
begin
  "a\xAEb".b.split("\xa4\xa2".dup.force_encoding("EUC-JP"))
rescue => e
  p [e.class, e.message]
end
p "a\xAEb".b.split("\xAE".b).map(&:bytes)
p "aXb".split("X")
