# CRuby names an enumerator after what built it, arguments and all.  koruby only
# knew the desc string a BUILTIN enumerator carries, so every enum_for/to_enum
# one printed the generator Proc instead ("#<Enumerator: #<Proc:0x…>>") and the
# builtins dropped their arguments.  ostruct's test asserts
#   '#<Enumerator: #<OpenStruct name="John Smith", …>:each_pair>'
require 'ostruct'
def norm(s) = s.gsub(/0x\h+/, "0xA").sub(%r{ /[^ >]+\.rb:\d+}, " F")
p norm([1, 2, 3].each.inspect)
p norm([1, 2, 3].each_slice(2).inspect)
p norm([1, 2, 3].each_cons(2).inspect)
p norm({ a: 1 }.each.inspect)
# ({a: 1}.each_pair / #each_entry are the SAME method as #each here, and koruby
#  has no invoked-name accessor in a cfunc, so their enumerator says "each".)
p norm("ab".each_char.inspect)
p norm((1..3).each.inspect)
p norm([1, 2].map.inspect)
p norm([1, 2].each_with_index.inspect)
p norm([1, 2].each_with_object([]).inspect)
p norm([3, 1, 2].sort_by.inspect)
p norm([1, 2].select.inspect)
p norm([1, 2].reject.inspect)
p norm([1, 2].find.inspect)
p norm(OpenStruct.new(a: 1, b: "x").each_pair.inspect)
p norm(Enumerator.new { |y| y << 1 }.inspect)
p norm([1, 2].lazy.inspect)
p norm(2.times.inspect)
p norm([1, 2].to_enum(:each_slice, 2).inspect)
p norm([1, 2].enum_for(:each_with_object, {}).inspect)
p [Enumerator::Generator.is_a?(Class), Enumerator.const_defined?(:Generator)]
# #to_s is #inspect
e = [1, 2].each_slice(2)
p [norm(e.to_s) == norm(e.inspect), e.to_a]
