# String#* with a 0 count must not write the source bytes past a 0-byte payload
p "CamelCase" * 0
p "" * 5
p "ab" * 1
p "abc" * 3
p "x" * 0
p ("é" * 0).encoding
p ("abc" * 2).frozen?
p ("a" * 1000).size
