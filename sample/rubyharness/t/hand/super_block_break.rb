# `super { break v }` ends the super call with v (the break the literal block raised belongs
# to that call site), also inside a lambda; a block converted to a Proc by the super
# method breaks as an orphan.
cls1 = Class.new { def foo; yield; end; def bar(a) = yield(a) }
cls2 = Class.new(cls1) { def foo; super { break 1 }; end; def bar(a) = super(a) { |x| break x * 3 } }
p cls2.new.foo, cls2.new.bar(4)
cls3 = Class.new { def foo(&b); b; end }
cls4 = Class.new(cls3) { def foo; super { break 1 }.call; end }
p((cls4.new.foo rescue $!.class))
p(-> { cls2.new.foo }.call)
