class D
  def initialize = @nb = false
  def get = { x: 2 }
  def set(v) = v.each { |k, x| p [k, x] }
  def cleanup = nil
  def spawn_fiber
    fiber_vars = get
    Fiber.new(blocking: !@nb) do
      set(fiber_vars)
      yield
    ensure
      cleanup
    end
  end
  def clobber(n) = n > 0 ? clobber(n - 1) + [n, n.to_s, {n => n}].size : 0
  def run
    fs = 3.times.map { |i| spawn_fiber { [:yielded, i] } }
    clobber(50)
    fs.map(&:resume)
  end
end
p D.new.run
class E
  def get = { x: 2 }
  def set(v) = v.each { |k, x| x }
  def spawn_fiber
    fiber_vars = get
    Fiber.new do
      set(fiber_vars)
    end
  end
  def clobber(n) = n > 0 ? clobber(n - 1) + [n, n.to_s, {n => n}].size : 0
  def run1
    f = spawn_fiber
    clobber(50)
    f.resume
  end
  def run3
    fs = 3.times.map { |i| spawn_fiber }
    fs.map(&:resume)
  end
  def run3b
    fs = [spawn_fiber, spawn_fiber]
    fs.map(&:resume)
  end
end
p((E.new.run1 rescue $!.class), (E.new.run3b rescue $!.class), (E.new.run3 rescue $!.class))
