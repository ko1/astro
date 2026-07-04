# Default Object#inspect: "#<Class:0x.. @a=1, @b=2>" with ivars in definition
# order; #to_s omits ivars; cyclic refs render "...". Addresses masked. vs ruby.
def mask(s); s.gsub("0x", "@").split("@").map { |p| p =~ /\A\h/ ? "ADDR" + p.sub(/\A\h+/, "") : p }.join rescue s; end
class Plain; end
class WithIvars; def initialize; @x = 1; @name = "bob"; @arr = [1, 2]; end; end
class Cyc; def initialize; @me = self; end; end
p WithIvars.new.inspect.include?('@x=1, @name="bob", @arr=[1, 2]')
p Plain.new.inspect.include?("@")
p WithIvars.new.to_s.include?("@x")
p Cyc.new.inspect.include?("...")
