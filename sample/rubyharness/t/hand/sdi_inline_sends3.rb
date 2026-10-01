# Third batch of AOT inline sends (korb_sdi_tab): op-assign `a[i] += x` desugars to [] / []=
# sends, plus ** / to_f / to_int / rotate! / chomp! / byteslice / +@ / rindex / ascii_only? /
# insert / b / =~ / casecmp / ord / rjust / keys / instance_variable_get / delete_at / reverse!.
# Out-of-range, frozen, subclass, Hash default and later redefinition; interp and AOT must match CRuby.
def run(xs, &b) = xs.map { |x| begin; b.call(x); rescue => e; e.class; end }
def bump(a, i) = (a[i] += 1)
def bump2(a, i, j) = (a[i, j] += [9])
a = [1, 2, 3]
p bump(a, 0), bump(a, -1), a, (bump(a, 5) rescue $!.class), a
h = Hash.new(10)
p bump(h, :k), bump(h, :k), h
s = +"ab"
p (s[0] += "z"), s, (bump(s, 0) rescue $!.class)
p bump2([1, 2, 3], 0, 2), bump2(+"abc", 1, 1) rescue p $!.class
fz = [1].freeze
p (bump(fz, 0) rescue $!.class), fz
class SubA < Array
  def [](i) = :sub_aref
  def []=(i, v)
    super(i, v.to_s)
  end
end
sa = SubA.new([1, 2])
p (bump(sa, 0) rescue $!.class), sa.to_a
p run([2, -2, 2.5, 10**20, 0]) { [_1 ** 2, _1 ** -1, _1 ** 0.5] }
p run([3, 2.5, 10**20, "4.5x", "", nil]) { [_1.to_f, _1.to_f.class] }
p run([3, 2.5, 10**20]) { _1.to_int }
p run([[1, 2, 3]]) { |x| [x.rotate!(1), x.rotate!(-4), x.delete_at(1), x.delete_at(9), x.insert(1, :i), x.reverse!, x.rindex(:i)] }
p run([+"ab\n", +"ab", +"ab\r\n"]) { [_1.chomp!, _1] }
p run(["héllo", "abc"]) { [_1.byteslice(1, 2), _1.byteslice(-2, 9), _1.ascii_only?, _1.b.encoding, _1.ord, _1.rjust(8, "*"), _1.rindex("l")] }
p run(["fz".freeze, +"m"]) { x = +_1; [x.frozen?, x.equal?(_1)] }
p run(["abc", /b/]) { [_1 =~ /c/, (_1 =~ "abc" rescue $!.class)] }
p run(["aBc", :aBc]) { [_1.casecmp("ABC"), _1.casecmp(:abc), _1.casecmp(1)] }
p run([+"abc"]) { [_1.insert(1, "Z"), _1.reverse!] }
p run([{ a: 1, b: 2 }, {}]) { _1.keys }
o = Object.new
o.instance_variable_set(:@x, 5)
p run([o, 1, "s"]) { [_1.instance_variable_get(:@x), _1.instance_variable_get(:@nope)] }
def loop_site(xs) = xs.map { |x| [x[0], (x ** 2 rescue $!.class), (x.to_f rescue $!.class), (x.keys rescue $!.class)] }
p loop_site([[4, 5], 3, { z: 1 }])
class Array
  def [](i) = :redef_aref
end
class Integer
  def **(x) = :redef_pow
  def to_f = :redef_to_f
end
class Hash
  def keys = :redef_keys
end
p loop_site([[4, 5], 3, { z: 1 }]), bump([1], 0) rescue p $!.class
