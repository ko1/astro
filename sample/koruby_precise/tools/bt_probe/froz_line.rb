# Where a FrozenError is reported.  A write refused inside a C method is placed
# at that method's call site by the dispatch; an INLINE write (a[i]=, a << x,
# obj.attr=) has no dispatch to do that and used to carry line 0 into the
# backtrace.  Run under CRuby and koruby and diff.
def bt(t)
  yield
rescue => e
  puts "#{t}: #{e.backtrace.first.to_s.gsub(__dir__ + '/', '')}"
end

class K
  def set(v); @x = v; end
  def sing; def self.zz; end; end
end

bt('1 a[i]=')    { a = [1].freeze; a[0] = 2 }
bt('2 a<<')      { a = [1].freeze; a << 2 }
bt('3 ivar')     { o = K.new.freeze; o.set(1) }        # KNOWN GAP: koruby says line 0
bt('4 def sing') { o = K.new.freeze; o.sing }          # KNOWN GAP: koruby says line 0
bt('5 attr=')    { s = Struct.new(:z).new(1).freeze; s.z = 2 }
bt('6 ivar imm') { 1.instance_variable_set(:@a, 1) }
bt('7 h[k]=')    { h = {}.freeze; h[:a] = 1 }          # KNOWN GAP: koruby adds a 'Hash#[]=' frame
bt('8 str<<')    { s = "a".freeze; s << "b" }          # KNOWN GAP: koruby adds a 'String#<<' frame
bt('9 a.push')   { a = [1].freeze; a.push(2) }
