# String#lines/each_line chomp: keyword strips the line separator. vs ruby.
p "a\nb\nc\n".lines
p "a\nb\nc\n".lines(chomp: true)
p "a\nb\nc".lines(chomp: true)
p "a-b-c".lines("-")
p "a-b-c".lines("-", chomp: true)
r = []; "x\ny\n".each_line(chomp: true) { |l| r << l }; p r
r = []; "x\ny\n".each_line { |l| r << l }; p r
p "hello world".lines
