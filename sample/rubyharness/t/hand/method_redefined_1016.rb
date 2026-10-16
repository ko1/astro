# "method redefined; discarding old m" (+ "previous definition of m was here"): def / attr / define_method / alias over
# a method of the same class; none after alias, undef, remove_method, or for an inherited one (2026-10-16)
$log = []
module Warning
  def self.warn(msg, **) = $log << msg.gsub(/\S*\//, "")
end
$VERBOSE = true
class A
  def a; end
  def a; end
  attr_reader :b
  def b; end
  def c; end
  attr_reader :c
  define_method(:d) {}
  def d; end
  def e; end
  define_method(:e) {}
  def f; end
  alias g f
  def f; end
  def g; end
  def h; end
  def i; end
  alias h i
  def to_s; end
  private def j; end
  def j; end
  def k; end
  undef_method :k
  def k; end
  def l; end
  remove_method :l
  def l; end
  def m; end
  public :m
  def m; end
end
class B < A; def a; end; end
class String; def size; end; end
o = Object.new
def o.x; end
def o.x; end
module M; def self.y; end; def self.y; end; end
def top; end
def top; end
class A; def n; end; end
class A; def n; end; end
$VERBOSE = false
class A; def q; end; def q; end; end
$VERBOSE = true
class A; alias_method :r, :a; def r; end; alias_method :s, :a; end
class A; def s; end; end
puts $log
