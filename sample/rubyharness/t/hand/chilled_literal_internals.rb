# koruby's own Ruby (the prelude) must not unchill a user's literal just by
# reflecting on it: CRuby's C implementations cannot, and the warning under -w
# would reach the user.
s = "x"
p Marshal.load(Marshal.dump(s))
p s.frozen?
p (s.singleton_method(:nope) rescue [$!.class, $!.message[0, 46]])
o = Object.new
p (o.singleton_method(:zz) rescue [$!.class, $!.message[0, 34]])
def o.yy = 2
p o.singleton_method(:yy).call
p (nil.singleton_method(:to_s) rescue [$!.class, $!.message[0, 34]])
class C; def self.cm = 3; end
p [C.singleton_method(:cm).call, (C.singleton_method(:nope) rescue $!.class)]
module M; def self.mm = 4; end
p M.singleton_method(:mm).call
# a singleton that DOES exist is still refused by Marshal
o2 = Object.new
def o2.hi = 1
p (Marshal.dump(o2) rescue [$!.class, $!.message[0, 30]])
o3 = Object.new
o3.extend(Comparable)
p (Marshal.dump(o3) rescue [$!.class, $!.message[0, 30]])
p Marshal.load(Marshal.dump({ "k" => [1, "v"] }))
