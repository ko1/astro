# A block's backtrace label names its OWNER.  Three sources answer that:
# a class/module/singleton body is settled at parse time (node_entry.blk_label),
# the file top level bakes only its depth (blk_top_level), and a METHOD owner is
# a run-time question — CRuby uses the owner the `def` had when it ran, not the
# lexical one, so `Bar.class_eval { def ce; }` is "Bar#ce".
#
# The hard case is a block called after its defining frame returned: only the
# heap still names the owner.  Run under CRuby and koruby and diff.
def show(p) = p.call

def meth; proc { caller_locations(0).first.label }; end
def run(p) = p.call
p ["method owner, frame returned", run(meth)]

class Foo; def m2; lambda { caller_locations(0).first.label }; end; end
p ["class method owner", run(Foo.new.m2)]
p ["top-level owner", run(proc { caller_locations(0).first.label })]

module M; def mm; proc { caller_locations(0).first.label }; end; end
class C; include M; end
p ["include: the DEFINING module", show(C.new.mm)]

class D; def dm; proc { caller_locations(0).first.label }; end; end
class E < D; end
p ["inherited: the defining class", show(E.new.dm)]

class I; def im; proc { caller_locations(0).first.label }; end; alias_method :im2, :im end
p ["alias: the original name", show(I.new.im2)]

class Foo2; def self.make; proc { caller_locations(0).first.label }; end; end
p ["def self.x", show(Foo2.make)]
o = Object.new
def o.sm; proc { caller_locations(0).first.label }; end
p ["def obj.x: no owner prefix", show(o.sm)]

class Bar; end
Bar.class_eval do
  def ce; proc { caller_locations(0).first.label }; end
end
p ["def inside class_eval block", show(Bar.new.ce)]

def deep2; [1].map { proc { caller_locations(0).first.label } }.first; end
p ["two block levels in a method", run(deep2)]
class Qux; def qm; proc { proc { caller_locations(0).first.label } }.call; end; end
p ["two levels in a class method", run(Qux.new.qm)]

# the block chain must not cost the locals walk its staleness check
def m_uncaptured
  gone = 5
  pr = proc { __frame_local_get(:gone) }
  [pr.call, pr]
end
v, pr = m_uncaptured
p ["live outer frame reachable by name", v]
p ["returned scope is gone", (begin; pr.call; "REACHED"; rescue NameError; "NameError"; end)]
