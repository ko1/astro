# L2: subclassing builtin types + extend / singleton methods
# (oracle = CRuby)

# --- String subclass ---
class MyStr < String
  def shout
    upcase + "!"
  end
end

s = MyStr.new("hello")
p s.class
p s.is_a?(String)
p s.is_a?(MyStr)
p s.length            # inherited builtin method
p s.upcase            # inherited builtin method
p s.shout             # own method
p(s + " world")       # builtin operator still works
p s.kind_of?(Comparable) rescue p "no Comparable"

# --- Array subclass ---
class Stack < Array
  def peek
    last
  end
end

st = Stack.new
st.push(1)
st.push(2)
st.push(3)
p st.class
p st.is_a?(Array)
p st.peek             # own method
p st.length           # inherited
p st.map { |x| x * 2 } # inherited enumerable

# --- Hash subclass ---
class Config < Hash
  def required(k)
    fetch(k)
  end
end

c = Config.new
c[:host] = "localhost"
p c.class
p c[:host]
p c.required(:host)
p c.keys

# --- extend a module onto an instance ---
module Greeter
  def greet
    "hi, #{self}"
  end
end

obj = "world"
obj.extend(Greeter)
p obj.greet
p obj.class           # still String (singleton class is transparent to .class)
p "plain".respond_to?(:greet) rescue p false

# --- singleton method on an instance ---
arr = [1, 2, 3]
def arr.total
  sum
end
p arr.total
p [9, 9].respond_to?(:total) rescue p false
