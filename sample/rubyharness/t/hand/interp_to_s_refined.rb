# A refined #to_s reaches interpolation too (dry-logger's specs refine Hash#to_s
# to the pre-3.4 format).
module M
  refine ::Hash do
    def to_s = map { |k, v| "#{k.inspect}=>#{v.inspect}" }.then { |ps| "{#{ps.join(",")}}" }
  end
end
using M
h = { a: 1, b: "x" }
p h.to_s
p "#{h}"
p h.inspect
def f(x) = "#{x}"
p f(h)
p [h].to_s
