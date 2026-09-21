# The inline-cache call path dropped the callee's method entry on its cold
# return tail, so the SECOND (cache-hit) call of a method that raises got a
# backtrace frame with no owner and the CALL SITE's line instead of the raise's:
#   1st: "f.rb:6:in 'Object#lvl3'"   2nd: "f.rb:7:in 'lvl3'"
# The entry is already in the callee frame's identity cell (base[-4]); the cold
# tail reads it back, so the hot path is untouched.
def cap
  yield
rescue => e
  e
end
def lvl3 = raise("boom")
def lvl2 = lvl3
def a = lvl2
def b = lvl2
def bt(e) = e.backtrace.map { |l| l.sub(/.*backtrace_repeat_call\.rb:/, ":") }.first(5)

3.times { p bt(cap { a }) }
3.times { p bt(cap { b }) }

class K
  def raiser = raise(ArgumentError, "k")
  def mid = raiser
end
k = K.new
3.times { p bt(cap { k.mid }) }

# through a block and a define_method'd method
define_method(:dm) { lvl2 }
3.times { p bt(cap { dm }) }
3.times { p bt(cap { [1].map { lvl2 } }) }

# a C method that raises, called repeatedly
3.times { p bt(cap { Integer("zz") }) }
3.times { p bt(cap { [].fetch(3) }) }
