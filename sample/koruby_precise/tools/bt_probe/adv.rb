# Adversarial caller/backtrace scenarios: frames reached through C-built windows,
# stale carries, rescue-modifier capture, bind_call, yields from inside blocks.
def show(tag, list)
  puts "== #{tag}"
  (list || ['<nil>']).each { |l| puts "  #{l}" }
end

# 1. to_s called from C (puts) with an Integer arg staged just below the receiver
class ToS; def to_s; show('1 puts to_s', caller(0)); 'ts'; end; end
puts 9, ToS.new, 12345678901

# 2. inspect from p
class Insp; def inspect; show('2 p inspect', caller(0)); 'i'; end; end
p 7, Insp.new

# 3. hash/eql? from Hash#[]=
class HK
  def hash; show('3 hash', caller(0)); 1; end
  def eql?(o); show('3 eql?', caller(0)); true; end
end
h = {}; h[HK.new] = 1; h[HK.new]

# 4. <=> from sort with integer args nearby
class Cmp
  include Comparable
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(o); show("4 <=> #{@v}", caller(0)) if @v == 1; @v <=> o.v; end
end
[Cmp.new(2), Cmp.new(1)].sort
[Cmp.new(1), Cmp.new(2)].max

# 5. == from Array#include? / index
class Eq; def ==(o); show('5 ==', caller(0)); false; end; end
[Eq.new].include?(3)
[1, 2, 3].index(Eq.new)

# 6. method_missing / respond_to_missing? driven from C (respond_to?)
class MM
  def method_missing(n, *a); show("6 mm #{n}", caller(0)); super if n == :zzz; 1; end
  def respond_to_missing?(n, p = false); show("6 rtm #{n}", caller(0)); true; end
end
MM.new.respond_to?(:foo)
MM.new.bar(1, 2)
begin; MM.new.zzz; rescue NoMethodError => e; show('6 zzz bt', e.backtrace); end

# 7. bind_call / bind.call / Method#call / UnboundMethod
class BC; def m(a, b = 2); pr = proc { a }; show("7 m #{a} #{b}", caller(0)); [pr.call, block_given?]; end; end
um = BC.instance_method(:m)
p um.bind_call(BC.new, 42)
p um.bind_call(BC.new, 43, 44)
p um.bind(BC.new).call(45)
p BC.new.method(:m).call(46)
p BC.new.method(:m).to_proc.call(47)

# 8. stale carry: yield with no block (LocalJumpError) then a block frame
def y0; yield; end
def go8; y0; rescue LocalJumpError; [1].each { show('8 after LJE', caller(0)) }; end
go8

# 9. stale carry: lambda arity error inside yield
def y2; yield 1, 2; end
def go9
  y2(&->(a) { a })
rescue ArgumentError
  [1].each { show('9 after lambda arity', caller(0)) }
end
go9

# 10. yield from inside a block (yield_outer) through a C iterator
def m10; [1].each { yield }; end
m10 { show('10 yield_outer', caller(0)) }

# 11. rescue modifier capture
def m11(a, b)
  e = (raise("x") rescue $!)
  show('11 rescue modifier', e.backtrace)
end
m11(1, 2)

# 12. splat calls
def s12(*a); show('12 splat', caller(0)); end
s12(*[1, 2])
obj = Object.new
def obj.s12b(*a); show('12 splat recv', caller(0)); end
obj.s12b(*[3])
caller(*[]).then { |l| show('12 caller splat', l) }

# 13. send / public_send / __send__ / Symbol#to_proc / define_method
def s13; show('13 send', caller(0)); end
send(:s13)
begin; public_send(:s13); rescue NoMethodError; end
[1].each(&:s13) rescue show('13 sym proc', $!.backtrace)
class DM; define_method(:dm) { |x| show('13 dm', caller(0)) }; end
DM.new.dm(1)
DM.new.method(:dm).call(2)

# 14. yield to a Symbol proc / Method proc
def y14; yield 1; end
def s14(x); show('14 yield symproc', caller(0)); end
y14(&method(:s14))
class Y14; def s14; show('14 yield symproc2', caller(0)); end; end
def y14b; yield Y14.new; end
y14b(&:s14)

# 15. class body, Class.new, instance_eval, at_exit
class C15; show('15 class body', caller(0)); [1].each { show('15 class body blk', caller(0)) }; end
Class.new { show('15 Class.new', caller(0)) }
Object.new.instance_eval { show('15 instance_eval', caller(0)) }
at_exit { show('15 at_exit', caller(0)) }

# 16. exception raised in C (Integer("x")), from a nested Ruby frame
def m16; Integer("zz"); end
begin; m16; rescue ArgumentError => e; show('16 C raise', e.backtrace); end

# 17. deep recursion + caller(0).size
def r17(n); n == 0 ? caller(0).size : r17(n - 1); end
puts "== 17 depth #{r17(50)}"

# 18. raise from inside a block inside a method, rescued outside
def m18; [1].each { raise 'b' }; end
begin; m18; rescue => e; show('18 raise in block', e.backtrace); end
