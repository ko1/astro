# Refinements are lexical: a call site outside `using` keeps its method cache
# while another file has refinements on, and a site under `using` still sees
# the refinement every time — also after the refined method is redefined, from
# blocks, and across calls that alternate between refined and plain scopes.
class Pt
  def initialize(x) = @x = x
  def show = "pt#{@x}"
end
def plain_show(o) = o.show          # defined before any refinement exists

module R
  refine Pt do
    def show = "refined#{super}"
  end
end

def plain_show2(o) = o.show         # after `refine`, but no `using` here

module UsesR
  using R
  def self.show(o) = o.show
  def self.show_in_block(o) = [o].map { |x| x.show }.first
  def self.call_plain(o) = plain_show(o)   # the callee's site is not under `using`
end

pts = Array.new(3) { |i| Pt.new(i) }
3.times do
  p pts.map { |o| [plain_show(o), plain_show2(o), UsesR.show(o), UsesR.show_in_block(o), UsesR.call_plain(o)] }.first
end
class Pt
  def show = "new#{@x}"             # redefinition reaches both kinds of site
end
p [plain_show(pts[1]), UsesR.show(pts[1]), UsesR.call_plain(pts[1])]
module R
  refine Pt do
    def show = "again#{super}"
  end
end
p [plain_show(pts[2]), UsesR.show(pts[2])]
# a method that is only defined long after its call site first ran
class Late; end
def call_late(o) = (o.late rescue :none)
p call_late(Late.new)
class Late; def late = :late; end
p call_late(Late.new)
# define_method / alias / attr on names already called through a cache
class Late
  define_method(:late) { :dm }
end
p call_late(Late.new)
class Late
  def other = :other
  alias_method :late, :other
end
p call_late(Late.new)
class Late
  attr_accessor :late
end
l = Late.new; l.late = :attr
p call_late(l)
