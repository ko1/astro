# Encoding drives character-level ops (CSI-style): UTF-8 counts code points,
# US-ASCII / ASCII-8BIT count bytes. vs ruby. (Shift_JIS etc. raise
# NotImplementedError in koruby but Ruby supports them, so not diffed here.)
p "café".length                          # 4 (UTF-8 chars)
p "café".bytesize                        # 5
p "café".reverse                         # "éfac"
p "\xE3\x81\x82".b.length                 # 3 (1 byte = 1 char)
p "\xE3\x81\x82".b.bytesize               # 3
p "\xE3\x81\x82".b.chars.size             # 3
p "あ".b.length                           # 3
p "あ".b.reverse.bytes                    # [0x82, 0x81, 0xe3]
p "abc".force_encoding("US-ASCII").length # 3
p "abcd".force_encoding("US-ASCII").reverse # "dcba"
p "café".chars.size                       # 4
p "café".each_char.to_a.size              # 4
p "abc".b.length
p "".length
p "".b.length
