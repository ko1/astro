# Implicit #to_ary conversion honours #respond_to? and a user #method_missing
# (rb_check_funcall): Array#flatten / #product / #[]= multi-element rhs.
bo = BasicObject.new
p [bo].flatten.equal?(nil), [bo].flatten.size
def bo.method_missing(name, *) = [1, 2]
p [bo].flatten

class MM
  def initialize(*a) = @a = a
  def method_missing(name, *args)
    return @a if name == :to_ary
    super
  end
  def respond_to_missing?(name, priv = false) = name == :to_ary || super
end
p [1].product(MM.new(2, 3))
p [[MM.new(4)]].flatten
p [MM.new(5, 6), 7].flatten(1)

class Decline
  def method_missing(name, *args) = raise(NoMethodError, "nope: #{name}")
end
d = Decline.new
p [d].flatten.size, [d].flatten[0].equal?(d)
a = [1, 2]
a[0, 0] = d
p a.size, a[0].equal?(d)

obj = Object.new
def obj.to_ary = [1, 2, 3]
ary = [1, 2]
ary[0, 0] = obj
p ary
ary[1, 10] = obj
p ary
ary = [0, 1, 2, 3]
ary[1..2] = obj
p ary
ary = [0, 1]
ary[0, 1] = nil
p ary

n = Object.new
def n.to_ary = nil
ary = [1]
ary[0, 1] = n
p ary[0].equal?(n)

bad = Object.new
def bad.to_ary = 42
begin
  [1][0, 1] = bad
rescue TypeError => e
  puts e.message
end
