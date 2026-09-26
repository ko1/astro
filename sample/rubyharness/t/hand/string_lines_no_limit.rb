# String#each_line / #lines take a separator (and chomp:) but no limit, unlike IO#each_line.
p(("ab".lines("a", 1) rescue [$!.class, $!.message]))
p(("ab".each_line("a", 1).to_a rescue [$!.class, $!.message]))
p(("".lines("a", 1) rescue $!.class))
p "ab".lines("a", chomp: true), "ab".each_line("b").to_a, "a\nb".lines(chomp: true)
