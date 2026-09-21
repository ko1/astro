# `raise SomeError.new(...)` / `raise SomeError, "msg"` never went through the
# C-level korb_raise, which is the only place that started a FRESH unwind list.
# So the backtrace captured at the rescue was prepended with whatever an earlier
# (already handled) raise had left in vm->bt — in a fresh process that is the
# prelude's own `Kernel#require` frames, and every later backtrace then repeated
# the first one's contents.
def cap
  yield
rescue Exception => e
  e
end
def bt(e) = e.backtrace.map { |l| l.sub(/.*backtrace_raise_object\.rb:/, ":") }.first(4)
def deep(exc) = raise(exc)
def wrap1(exc) = deep(exc)
def wrap2(exc) = deep(exc)

p bt(cap { wrap1(RuntimeError.new("a")) })
p bt(cap { wrap2(RuntimeError.new("b")) })
pre = ArgumentError.new("c")
p bt(cap { wrap1(pre) })
p bt(cap { raise(TypeError.new("d")) })
p bt(cap { raise(KeyError, "e") })
p bt(cap { raise("plain string") })
p bt(cap { wrap2(IndexError.new("f")) })
class E2 < StandardError; end
p bt(cap { wrap1(E2.new) })
p bt(cap { raise E2 })

# a re-raise keeps the ORIGINAL backtrace
first = cap { wrap1(RuntimeError.new("keep")) }
again = cap { raise first }
p [bt(first) == bt(again), bt(again)]

# and a raise inside a rescue gets its own
p bt(cap { begin; wrap1(RuntimeError.new("x")); rescue; wrap2(ArgumentError.new("y")); end })
