# pack("m0") = base64 with no line breaks / trailing newline; "m"/"m1"/"m2" wrap
# at 45 bytes; "mN" (N>=3) at N. vs ruby.
p ["hello"].pack("m0")
p ["hello"].pack("m")
long = "a" * 60
p [long].pack("m")
p [long].pack("m1")
p [long].pack("m0")
p ["abcdefghi"].pack("m3")
p [""].pack("m0")
