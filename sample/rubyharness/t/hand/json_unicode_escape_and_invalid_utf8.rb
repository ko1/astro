# JSON.parse of \uXXXX escapes (the digit after the escape was emitted twice),
# and JSON.generate refusing malformed UTF-8.
require "json"
bs = 92.chr
p JSON.parse('["' + bs + 'u003c/script>"]'), JSON.parse('"' + bs + 'u0041B"'), JSON.parse('"x' + bs + 'ud83d' + bs + 'ude00y"')
p JSON.parse('{"a":"' + bs + 'u00e9' + bs + 'n"}')
begin; p JSON.pretty_generate("a" => "\xFA"); rescue => e; p e.class; end
begin; p JSON.generate(["\xFF".b]); rescue => e; p e.class; end
p JSON.generate("é".encode("ISO-8859-1")), JSON.generate("ab".b), JSON.generate(:"xé")
