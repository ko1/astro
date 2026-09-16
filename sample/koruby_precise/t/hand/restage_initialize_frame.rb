# A Struct / Data / Exception subclass with a user-defined #initialize gets its
# callee frame re-staged ABOVE the window the generic send laid out.  The cells
# below that second frame are live caller slots, so its header (identity, link,
# EP) has to be staged too: a stale even VALUE left in the EP cell is read back
# as an open KorbEnv and dereferenced (DOOM segfaulted on a `false` argument).

S = Struct.new(:a, :b) do
  def initialize(a, b)
    super
  end
end
p S.new(1, false).to_a
p S.new(false, false).to_a
p S.new(2.5, 3.5).to_a          # flonum: even too
p S.new("x", :y).to_a

# a closure made in the re-staged frame must still close over it correctly
C = Struct.new(:v) do
  def initialize(v)
    @f = -> { v * 2 }
    super(v)
  end
  def f = @f.call
end
p C.new(21).f
p C.new(false).v

D = Data.define(:x, :y) do
  def initialize(x:, y:)
    super
  end
end
p D.new(x: 1, y: false).to_h
p D.new(false, false).to_h      # positional form -> keyword hash

class E < StandardError
  def initialize(m = "boom")
    super
  end
end
begin; raise E;           rescue => e; p [e.class, e.message]; end
begin; raise E, "hi";     rescue => e; p [e.class, e.message]; end
begin; raise E.new(false); rescue => e; p [e.class, e.message]; end

# deep enough that a stale cell from one call is the header of the next
100.times { |i| S.new(i.even?, false) }
p :ok
