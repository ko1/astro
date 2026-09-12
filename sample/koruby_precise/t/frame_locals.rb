# korb_frame_local_get/set — locals reached by NAME through the frame identity
# (docs/todo.md "ローカル名前表を ISEQ 入口へ").  Compared against `binding`,
# which answers the same questions from a table baked at the call site.
$ok = 0
$ng = 0
def eq(label, got, want)
  if got == want
    $ok += 1
  else
    $ng += 1
    puts "NG #{label}: got #{got.inspect}, want #{want.inspect}"
  end
end

# 1. a method's own locals
def m_own(a, b)
  x = a + b
  y = "s"
  eq("method arg a", __frame_local_get(:a), a)
  eq("method arg b", __frame_local_get(:b), b)
  eq("method local x", __frame_local_get(:x), x)
  eq("method local y", __frame_local_get(:y), y)
  eq("method vs binding", __frame_local_get(:x), binding.local_variable_get(:x))
  eq("method names", __frame_locals.sort, binding.local_variables.sort)
  eq("write back", __frame_local_set(:x, 99), 99)
  eq("write visible", x, 99)
end
m_own(1, 2)

# 2. a block's own locals
def m_block
  [10].each do |e|
    q = e * 2
    eq("block param e", __frame_local_get(:e), 10)
    eq("block local q", __frame_local_get(:q), 20)
    eq("block vs binding", __frame_local_get(:q), binding.local_variable_get(:q))
    __frame_local_set(:q, 7)
    eq("block write", q, 7)
  end
end
m_block

# 3. an enclosing method's local, seen from inside a block
def m_outer
  outer = 41
  [1].each do |i|
    eq("outer from block", __frame_local_get(:outer), 41)
    eq("outer vs binding", __frame_local_get(:outer), binding.local_variable_get(:outer))
    __frame_local_set(:outer, outer + 1)
    [2].each do |j|
      eq("outer from nested block", __frame_local_get(:outer), 42)
      eq("nested sees i", __frame_local_get(:i), 1)
      eq("nested sees j", __frame_local_get(:j), 2)
    end
  end
  eq("block write reached method", outer, 42)
end
m_outer

# 4. shadowing: the innermost name wins, as `binding` reports it
def m_shadow
  v = :method
  [1].each do |_|
    v = :block_assigned_same_slot
    eq("shadow (same var)", __frame_local_get(:v), binding.local_variable_get(:v))
  end
  [1].each do |v|
    eq("shadow (block param)", __frame_local_get(:v), 1)
    eq("shadow vs binding", __frame_local_get(:v), binding.local_variable_get(:v))
  end
end
m_shadow

# 5. a scope that has already returned, read through its captured (closed) env.
# The block must REFERENCE the local: koruby captures on demand, so a scope no
# closure reads is simply gone once its frame returns (see 5b).
def m_closed
  kept = 5
  pr = proc { kept
              eq("closed env read", __frame_local_get(:kept), 5)
              __frame_local_set(:kept, 6)
              eq("closed env write", __frame_local_get(:kept), 6)
              eq("closed env vs binding", __frame_local_get(:kept), binding.local_variable_get(:kept)) }
  pr
end
m_closed.call

# 5b. the same block WITHOUT the reference: nothing kept that scope alive, so
# the name is not reachable any more.  (While the defining frame is still live
# it is, through the raw PREV link — the first call below.)
def m_uncaptured
  gone = 5
  pr = proc { eq("live outer frame", __frame_local_get(:gone), 5) }
  pr.call
  pr
end
begin
  m_uncaptured.call
  eq("uncaptured scope is gone", false, true)
rescue NameError
  eq("uncaptured scope is gone", true, true)
end

# 6. define_method bodies are blocks at run time
class DMHost
  define_method(:dm) do |arg|
    local = arg * 3
    eq("define_method arg", __frame_local_get(:arg), 2)
    eq("define_method local", __frame_local_get(:local), 6)
  end
end
DMHost.new.dm(2)

# 7. a class body's locals
class CBody
  cb = 12
  eq("class body local", __frame_local_get(:cb), 12)
  eq("class body vs binding", __frame_local_get(:cb), binding.local_variable_get(:cb))
end

# 7b. module / singleton-class bodies, and a block inside a class body reaching
#     the body's own locals through the EP chain (these frames carry their own
#     identity now, so the walk names them)
module MBody
  mb = 7
  eq("module body local", __frame_local_get(:mb), 7)
  eq("module body names", __frame_locals.sort, binding.local_variables.sort)
end
class SBody
  class << self
    sb = 9
    eq("sclass body local", __frame_local_get(:sb), 9)
    eq("sclass body write", __frame_local_set(:sb, 10), 10)
    eq("sclass body write visible", sb, 10)
  end
end
class CBody2
  outer = 3
  [1].each do
    inner = outer + 1
    eq("block-in-class own local", __frame_local_get(:inner), 4)
    eq("block-in-class outer local", __frame_local_get(:outer), 3)
  end
end

# 7c. a define_method body reaching the class body's local it captured (the body
#     must READ it: an uncaptured scope is gone once the body frame returned —
#     the same hole as 5b)
class DMBody
  seed = 21
  define_method(:dm2) do
    seed
    eq("define_method captured class-body local", __frame_local_get(:seed), 21)
  end
end
DMBody.new.dm2

# 8. a name that is not in scope
def m_missing
  begin
    __frame_local_get(:nope)
    eq("missing raises", false, true)
  rescue NameError
    eq("missing raises", true, true)
  end
end
m_missing

puts "ok=#{$ok} ng=#{$ng}"
exit($ng == 0 ? 0 : 1)
