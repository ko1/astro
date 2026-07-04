# String#ord per the string's byte layout: single-byte tag -> first byte,
# UTF-8 -> first code point.  vs ruby.
p "cafe".ord            # 99
p "あ".ord          # 12354
p "あ".b.ord        # 227 (first byte)
p "\xFF".b.ord          # 255
p "A".force_encoding("US-ASCII").ord  # 65
