# Integer#chr(encoding) tags the result with the requested encoding (no arg:
# US-ASCII for 0..127, ASCII-8BIT otherwise). vs ruby.
p 65.chr(Encoding::ASCII_8BIT).encoding.name
p 65.chr(Encoding::US_ASCII).encoding.name
p 200.chr(Encoding::ASCII_8BIT).encoding.name
p 65.chr.encoding.name
p 200.chr.encoding.name
p 0x3042.chr(Encoding::UTF_8).bytes
