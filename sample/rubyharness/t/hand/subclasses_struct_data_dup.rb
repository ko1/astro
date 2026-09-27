# Struct.new / Data.define / Class#dup register the new class in its superclass's
# #subclasses; String#each_line takes no limit argument.
S = Struct.new(:a); D = Data.define(:x)
p Struct.subclasses.include?(S), Data.subclasses.include?(D)
S2 = Struct.new(:b); p Struct.subclasses.include?(S2)
c = Class.new(String); d = c.dup; p String.subclasses.include?(d)
p(("a\nb\nc".each_line("\n", 1) { } rescue [$!.class, $!.message]))
p("a\nb".each_line(chomp: true).to_a, "a\nb".each_line("\n", chomp: true) { |l| p l })
