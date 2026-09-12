# A block frame's backtrace link carries the line of the `yield` that placed it,
# and that line is SIGNED (eval(str, file, -100) is legal Ruby).  The carry
# travels as the packed 17-bit field, so a negative line must round-trip.
def each2
  yield
end

eval(<<~SRC, TOPLEVEL_BINDING, "negfile.rb", -100)
  def probe
    each2 { caller(0).first(2).map { |s| s.split("/").last } }
  end
SRC

p probe

# same, through a C-driven block (Array#each) rather than a Ruby yield
eval(<<~SRC, TOPLEVEL_BINDING, "negfile.rb", -50)
  def probe2
    [1].map { caller(0).first(2).map { |s| s.split("/").last } }
  end
SRC

p probe2
