class Thread
  class << self
    alias_method :_orig_new, :new
    def new(*a, &b) = _orig_new(*a) { |*x| [:wrapped, b.call(*x)] }
    alias_method :_orig_start, :start
  end
end
p Thread.new(1) { |x| x + 1 }.value
p Thread.method(:_orig_start).arity, Thread.singleton_class.instance_method(:_orig_new).owner
