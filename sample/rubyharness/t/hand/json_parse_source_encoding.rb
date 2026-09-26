# JSON.parse reads UTF-8: a binary source (what IO#read(n) gives an LSP server)
# yields UTF-8 strings, another encoding is transcoded first.
require 'json'
s = "[\"\xF0\x9F\x92\x8E\", {\"k\": \"v\"}]".b
r = JSON.parse(s)
p r[0].encoding, r[0] == "💎", r[1].keys[0].encoding
p JSON.parse("\"caf\xC3\xA9\"".b).encoding rescue p $!
p JSON.parse("\"x\"".encode("UTF-16LE")) rescue p $!.class
e = "[\"\xE9\"]".force_encoding("ISO-8859-1")
p JSON.parse(e)[0].encoding, JSON.parse(e)[0] rescue p $!.class
