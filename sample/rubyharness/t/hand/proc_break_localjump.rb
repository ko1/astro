# break in a `proc { }` / Proc.new block raises LocalJumpError where the proc is
# called (its target, the proc call, has returned) — rescuable there, not at
# <main>; a literal block turned into &b keeps breaking out of its call.
def m1; pr = proc { break :b }; pr.call; :after; end
p((m1 rescue [$!.class, $!.message]))
def callit(pr) = pr.call
def m2; pr = proc { break :b }; callit(pr); :after; end
p((m2 rescue [$!.class, $!.message]))
def yl = yield
def m3; pr = proc { break :b }; yl(&pr); :after; end
p((m3 rescue [$!.class, $!.message]))
def mk = proc { break :b }
p((mk.call rescue [$!.class, $!.message]))
p((callit(mk) rescue [$!.class, $!.message]))
l = lambda { break :lb; :no }
p l.call
def ll = [1, 2].each(&lambda { |x| break :lx })
p ll
def m(&b) = b.call
p m { break 5 }
def y = yield
p y { break 6 }
p((([1, 2].each(&proc { break :x })) rescue [$!.class, $!.reason, $!.exit_value]))
pr = proc { [1, 2].each { |i| break i * 10 } }
p pr.call
def w(&b) = [1, 2].map(&b)
p w { |x| break :early if x == 2; x }
l = ->(x) { break x * 2 }
p [1, 2].map(&l)
q = Proc.new { break :pn }
p((q.call rescue $!.class))
