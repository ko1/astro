# exception messages longer than a fixed C buffer are kept whole (method_source reads the tail of a SyntaxError)
p((Integer("z" * 700) rescue $!.message.size))
p((raise ArgumentError, "x" * 2000 rescue $!.message.size))
src = "def m\n" + (1..40).map { |i| "  x#{i} = <<~E#{i}\n  body\n  E#{i}\n" }.join
begin; eval(src); rescue SyntaxError => e; p e.message.lines.last.include?("unexpected end-of-input"); end
