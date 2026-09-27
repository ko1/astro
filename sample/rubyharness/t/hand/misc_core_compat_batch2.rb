module M; end
class Integer; prepend M; end
l = -> { 1 + 2 }
p l.call
M.module_eval { def +(o) = ($called = true; super(o)) }
p l.call, $called
module N; end; class String; include N; end
module N; def ==(o) = :n_eq; end
p("a" == "b")
m1 = Module.new { def calc(x) = x }
m2 = Module.new { prepend(m1) }
c1 = Class.new { prepend(m2) }
m2dup = m2.dup
p m2dup.ancestors == [m1, m2dup], m2dup.ancestors.size
c2 = Class.new { prepend(m2dup) }
p c1.ancestors[0, 3] == [m1, m2, c1], c1.new.is_a?(m1), c2.ancestors[0, 3] == [m1, m2dup, c2], c2.new.is_a?(m1)
p((0xD800.chr("UTF-8") rescue $!))
p((0xDFFF.chr(Encoding::UTF_8) rescue $!))
p 0xD7FF.chr("UTF-8"), 0xE000.chr("UTF-8"), 0x10FFFF.chr("UTF-8").bytes, 0x3042.chr("EUC-JP") rescue p $!
p((0xD800.chr("UTF-16LE") rescue $!))
p proc { |&| }.parameters, proc { |a, &| a }.call(3), lambda { |x, &| x * 2 }.call(4), proc { |&| 1 }.call { }
def m = yield(5); p m { |v, &| v + 1 }
