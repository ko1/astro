# Site caches stay on outside refined scopes once some `using` ran, and stay off
# inside them: h[k] / h[k]=v / case-when === / send(:sym) / a builtin's own helper call.
module HR
  refine Hash do
    def [](k) = "refined[#{k}]"
    def []=(k, v)
      super(k, "r#{v}")
    end
  end
  refine Integer do
    def ===(o) = o == :any || super
  end
  refine Array do
    def first = :refined_first
  end
end

class Plain   # defined before `using`: never sees HR
  def get(h) = h[:a]
  def set(h) = (h[:b] = 1; h[:b])
  def cw(x) = case x; when 5 then :five; else :other; end
  def snd(h) = h.send(:[], :a)
  def fst(a) = a.first
end

def run_plain(o)
  h = { a: 1 }
  [o.get(h), o.set(h), o.cw(:any), o.cw(5), o.snd(h), o.fst([9])]
end

pl = Plain.new
p run_plain(pl)
3.times { run_plain(pl) }

using HR

class Refined  # defined after `using`: sees HR
  def get(h) = h[:a]
  def set(h) = (h[:b] = 1; h[:b])
  def cw(x) = case x; when 5 then :five; else :other; end
  def snd(h) = h.send(:[], :a)
  def fst(a) = a.first
end

rf = Refined.new
4.times do
  p run_plain(pl)        # still plain
  h = { a: 1 }
  p [rf.get(h), rf.set(h), rf.cw(:any), rf.cw(5), rf.snd(h), rf.fst([9])]
end

# top-level (refined) sites interleaved with plain ones
h = { a: 2 }
5.times { |i| p [h[:a], pl.get(h), (h[:c] = i), pl.set(h)] }
