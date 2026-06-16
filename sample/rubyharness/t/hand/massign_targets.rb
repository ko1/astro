# multi-assign to heterogeneous targets (oracle = CRuby) — optcarrot needs these.

# constant targets
A1, A2, A3 = (1..3).map { |i| i * 10 }
p [A1, A2, A3]

# instance-variable targets (inside a method)
class Box
  def setup
    @x, @y, @z = 1, 2, 3
  end
  def vals
    [@x, @y, @z]
  end
  def from_array
    @a, @b = [7, 8]
  end
  def ab
    [@a, @b]
  end
end
b = Box.new
b.setup
p b.vals
b.from_array
p b.ab

# mixed local + ivar
class Mix
  def go
    local, @ivar = 100, 200
    [local, @ivar]
  end
end
p Mix.new.go
