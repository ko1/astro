# A #to_X that answers nil is "not convertible", not a type error: CRuby's
# rb_check_*_type family (IO#puts, Array.new's size form, Kernel#Hash) falls
# back, while the rb_convert_type family reports "gives nil".
def t(tag); r = yield; puts "#{tag}: => #{r.inspect}"; rescue => e; puts "#{tag}: #{e.class}: #{e.message}"; end
class A; def to_ary = nil; end
class AS < A; def to_s = "OBJ"; end   # only where #to_s is the point
class B; def to_str = nil; end
class C; def to_int = nil; end
class G; def to_hash = nil; end
require 'stringio'
t("io puts")        { io = StringIO.new(+""); io.puts(AS.new); io.string }
t("Array.new")      { Array.new(A.new) }
t("Hash()")         { Hash(G.new) }
t("Array()")        { Array(A.new).size }
t("flatten")        { [A.new].flatten.size }
t("Array.try_conv") { Array.try_convert(A.new) }
t("String.try_conv"){ String.try_convert(B.new) }
t("Hash.try_conv")  { Hash.try_convert(G.new) }
t("Int.try_conv")   { Integer.try_convert(C.new) }
t("ary+")           { [1] + A.new }
t("concat")         { [1].concat(A.new) }
t("replace")        { [1].replace(A.new) }
t("union")          { [1].union(A.new) }
t("str+")           { "x" + B.new }
t("pack")           { [1].pack(B.new) }
t("sub")            { "abc".sub("a", B.new) }
t("ary[]")          { [1][C.new] }
t("str*")           { "x" * C.new }
t("merge")          { {}.merge(G.new) }
t("to_h pair")      { {a: 1}.to_h { |k, v| A.new } }
