# Float#to_s: the shortest round-tripping digits (the fast path for 1e-4 <= |d| < 1e15
# and the general one must agree with CRuby)
vals = [0.1, 0.2, 0.1 + 0.2, 19.99, 4.35, 0.07, 1.0 / 3, 2.0 / 3, 100.0, 1200.0, 1e-4, 9.9999e-5,
        1e15, 999999999999999.9, 1e16, 123456789012345.6, 0.5, 1e22, 5e-324, Float::MAX, 1e-5,
        12.5, 0.001234, 3.14159, 2.718281828459045, 1234567.0, 0.000123, 99.99999999999999]
vals.each { |v| puts v.to_s, (-v).to_s }
s = +""
(1..2000).each { |i| s << (i / 100.0).to_s << " " << (i * 0.1).to_s << " " << (i / 7.0).to_s << "\n" }
puts s.hash == s.dup.hash, s.bytesize
puts s.lines.first(5)
puts s.lines.last(5)
puts [1.0e-4, 1.5e-4, 0.00015, 1e14, 1.5e14, 1e14 + 0.5].map(&:to_s).join(",")
