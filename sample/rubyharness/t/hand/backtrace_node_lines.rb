# Backtrace lines and frames for raises that come from inside an expression
# (an implicit conversion, a pattern, a constant path), and the block frame of
# an arity error.
def show(tag)
  yield
  puts "#{tag}: no exception"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
  e.backtrace.first(4).each { |l| puts "  #{l.sub(/\A.*\//, '')}" }
end

conv = Object.new
def conv.to_ary = raise(TypeError, "to_ary")
def conv.to_a = raise(TypeError, "to_a")
bad = Object.new
def bad.to_a = 1
def bad.to_hash = 1

show(:massign) {
  a, b = conv
}
show(:massign_gvar) {
  $g1, b = conv
}
show(:massign_nested) {
  (a, b), c = conv, 1
}
show(:massign_splat_rhs) {
  a, b = *conv
}
show(:array_splat) {
  x = [*bad]
}
show(:kw_splat) {
  x = [**bad]
}
show(:call_splat) {
  p(*bad)
}
class CvarLine
  show(:cvar) {
    @@nope += 1
  }
end
d = Object.new
def d.deconstruct_keys(keys) = nil
def d.deconstruct = nil
show(:hash_pattern) {
  case d
  in key:
  end
}
show(:array_pattern) {
  case d
  in [*, 1, *]
  end
}
show(:sclass) {
  class << 1
  end
}
ns = nil
show(:cpath_opassign) { ns::NotDefined += 1 }
show(:cpath_or) { ns::NotDefined ||= 1 }
show(:cpath_and) { ns::NotDefined &&= 1 }
show(:cpath_write) { ns::NotDefined = 1 }
show(:cpath_massign) { ns::NotDefined, x = 1, 2 }
fns = false
show(:cpath_false_opassign) { fns::NotDefined += 1 }
module CpathM; end
m = CpathM
show(:cpath_read_missing) { m::NotDefined += 1 }
m::Y ||= 3
m::Y &&= 4
m::Y += 1
m::Z, x = 5, 6
p [m::Y, m::Z]
show(:multiline_call) {
  nil
    .foo
}
show(:lambda_arity) { -> { it }.call }
show(:lambda_arity2) { ->(a, b = 1, c) {}.call(1) }
show(:lambda_kw) { ->(k:) {}.call(1) }
show(:proc_lambda) { lambda do; rescue; end.call(1) }
o = Object.new
o.define_singleton_method(:dm) { |a| }
show(:define_method_arity) { o.dm }
show(:destructure_param) { [conv].each { |(f, s)| } }
show(:auto_splat_param) { [conv].each { |f, s| } }

class Symbol
  alias_method :__orig_eqq, :===
  def ===(other) = raise(TypeError, "Symbol#===")
end
show(:symbol_when) {
  case :other
  when :expected
  end
}
show(:symbol_in) {
  case :other
  in :expected
  end
}
class Symbol
  alias_method :===, :__orig_eqq
end
case :a
when :a then p :restored
end
