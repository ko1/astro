# Built-ins that called other built-ins through a full send now call them directly, as CRuby's C does:
# a Hash's default proc (Proc#call redefinition not seen), gsub with a replacement Hash (plain tables looked up
# in C; subclasses / default procs / defaults still honoured), String#+@ on a frozen string (a String#dup
# override not seen). Encodings, frozenness and identity of the results checked; GC in between.
h = Hash.new { |hh, k| hh[k] = k.to_s * 2 }
lam = Hash.new(&->(hh, k) { "lam#{k}" })
tbl = { "&" => "&amp;", "<" => "&lt;", ">" => "&gt;" }
tbl_d = Hash.new("?").merge("a" => "A")
tbl_p = Hash.new { |_, k| "<#{k}>" }
class TSub < Hash; def [](k) = "sub:#{k}"; end
tsub = TSub.new
bin = { "\xff".b => "X" }
3.times do |i|
  GC.start if i == 1
  p h[:a], h[i], h.default(:q), lam[1], lam.default(2), h.key?(:q)
  p "a<b>&c".gsub(/[<>&]/, tbl), "abc".gsub(/./, tbl_d), "ab".gsub(/./, tbl_p), "ab".gsub(/./, tsub)
  p "x\xffy".b.gsub(/\xff/n, bin), "aaa".gsub(/a/, {}), "a<".encode("UTF-16LE").encode("UTF-8").gsub(/</, tbl).encoding
  f = "lit".freeze; m = +f; u = +"mut"
  p m, m.frozen?, m.equal?(f), u.frozen?, (+u).equal?(u), (+"x".encode("ASCII-8BIT")).encoding
end
class String; def dup = :custom_dup; end
p (+"frozen".freeze).class
class Proc; alias __c call; def call(*a) = [:wrapped, __c(*a)]; end
p Hash.new { |_, k| k * 3 }[4]
class Hash; def [](k) = :hash_aref_redefined; end
p "ab".gsub(/./, { "a" => "1" })
