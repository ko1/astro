# `break` inside the block given to Enumerator#each must return from that
# `each` call.  koruby's prelude Enumerator#each forwards the caller's literal
# block (`&block`) to the C driver, so the driver never OWNS the break — it was
# ending the generator drive and returning the (empty) collector instead, and
# the unconsumed break claim then killed the next unrelated `break`.
# dry-auto_inject reads a method's parameters with
# `MethodParameters.of(k, :initialize).each { |ps| break ps unless ... }`.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

E = Enumerator.new { |y| y << 1; y << 2; y << 3 }

t(:break_mid)     { E.each { |v| break v * 10 if v == 2 } }
t(:break_first)   { E.each { |v| break :first } }
t(:break_never)   { E.each { |v| v }.class }
t(:break_nil)     { E.each { |v| break if v == 1 } }
t(:after_break)   { [1, 2, 3].each { |v| break v * 100 if v == 2 } }
t(:infinite)      { Enumerator.new { |y| loop { y << :inf } }.each { break :stopped } }
t(:nested)        { E.each { |a| break E.each { |b| break a + b } } }
t(:to_enum_break) { [4, 5, 6].to_enum(:each).each { |v| break v if v == 5 } }
t(:with_index)    { E.each_with_index { |v, i| break [v, i] if i == 1 } }
t(:produce)       { Enumerator.produce(1) { |n| n + 1 }.each { |n| break n if n == 4 } }
t(:lazy_each)     { E.lazy.map { |v| v * 2 }.each { |v| break v if v == 4 } }
t(:still_works)   { E.to_a }
t(:next_after)    { e = Enumerator.new { |y| y << :a; y << :b }; [e.next, e.next] }
t(:method_break)  { def find_it(en) = en.each { |v| break v if v > 1 }; find_it(E) }
