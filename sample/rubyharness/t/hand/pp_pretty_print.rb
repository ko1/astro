# koruby vendors CRuby's pp.rb + prettyprint.rb, so PP is a real class (gems
# subclass it: pry's ColorPrinter < ::PP) and the wrapping matches CRuby.
pp({a: 1})            # Kernel#pp autoloads the real pretty-printer
p defined?(PP)
p PP.superclass

require 'pp'
pp({a: 1, b: [1, 2, 3]})
pp (1..40).to_a
pp({"k" => "v" * 40, "k2" => (1..10).to_a})
pp({a: {b: {c: {d: (1..20).to_a}}}})
pp([[1, 2], [3, 4]])
pp 1..5
pp :sym, "str", nil, true

s = +""
PP.pp([1, 2, 3], s)
p s
p PP.singleline_pp({a: 1}, +"")
p [1, 2, 3].pretty_inspect

class Bar
  def pretty_print(q)
    q.group(1, "<Bar", ">") { q.breakable; q.text("custom") }
  end
end
pp Bar.new

Point = Struct.new(:a, :b)
pp Point.new(1, "two")

h = {}
h[:self] = h
pp h
a = [1]
a << a
pp a

class Sub < PP
  def self.tag = "sub"
end
p Sub.tag
p Sub.ancestors.include?(PrettyPrint)
