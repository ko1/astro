# What Kernel#puts / Warning.warn actually call on a stand-in $stdout/$stderr,
# a user-defined #send, and the class names a comparison failure reports.
class Cap
  attr_reader :log
  def initialize = @log = []
  def write(*a) = (@log << [:write, a.map(&:to_s)]; a.sum { |x| x.to_s.length })
  def print(*a) = (@log << [:print, a.map(&:to_s)]; nil)
  def puts(*a)  = (@log << [:puts, a.map(&:to_s)]; nil)
  def <<(x)     = (@log << [:<<, [x.to_s]]; self)
  def flush = self
  def sync = true
  def sync=(v); v; end
  def tty? = false
end

# 1. CRuby's rb_f_puts forwards to $stdout.puts; #print and #<< go to their own.
o = $stdout
c = Cap.new
$stdout = c
puts "a"
print "b"
$stdout << "e"
$stdout = o
p c.log

# …and `super` out of a stand-in's own #puts must not recurse back into it.
class SelfPuts < Cap
  def puts(*a) = (@log << [:own, a.map(&:to_s)]; super)
end
o = $stdout
sp = SelfPuts.new
$stdout = sp
begin
  puts "z"
ensure
  $stdout = o
end
p sp.log

# 2. Warning.warn writes (rb_write_error_str), it does not print.
e = $stderr
c2 = Cap.new
$stderr = c2
warn "w"
Warning.warn("v\n")
$stderr = e
p c2.log

# 3. A class that defines its own #send wins over the reflective one, including
#    for an implicit-self call carrying a block (concurrent-ruby's Agent#send!).
class Sender
  def send(*args, &b) = [:mine, args, b ? b.call : nil]
  def implicit_args = send(1, 2)
  def implicit_blk  = send { 9 }
  def explicit      = self.send(3)
end
s = Sender.new
p [s.implicit_args, s.implicit_blk, s.explicit, s.send(:itself)]

# 4. A comparison failure names the operands' real class, not "Object".
class CmpNamed
  include Comparable
  def <=>(_o) = nil
end
def cmp(l)
  yield
rescue ArgumentError => ex
  p [l, ex.message.sub(/ failed.*/, " failed")]
end
cmp(:lt)   { CmpNamed.new < CmpNamed.new }
cmp(:sort) { [CmpNamed.new, CmpNamed.new].sort }
cmp(:min)  { [CmpNamed.new, CmpNamed.new].min }
cmp(:max)  { [CmpNamed.new, CmpNamed.new].max }

# 5. Psych reports invalid bytes as ArgumentError (vcr rescues exactly that).
require "yaml"
begin
  YAML.dump("a" => "\xC3\x28".dup.force_encoding("UTF-8"))
  p :no_raise
rescue => ex
  p [ex.class, ex.message]
end
