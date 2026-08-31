# Proc#curry — partial application.
class Proc
  def to_proc; self; end
  # Mark this Proc so a bare kwargs Hash flows through *args.  koruby already
  # threads a trailing kwargs Hash through splat, so the marking itself is a
  # no-op — but CRuby warns (and skips) when the shape cannot carry the flag:
  # no *rest, or any keyword/keyrest, or a post argument after the *rest.
  def ruby2_keywords
    ps = parameters
    rest = ps.index { |p| p[0] == :rest }
    ok = !rest.nil? &&
         ps.none? { |p| p[0] == :key || p[0] == :keyreq || p[0] == :keyrest } &&
         ps[(rest + 1)..].none? { |p| p[0] == :req || p[0] == :opt }
    unless ok
      warn "Skipping set of ruby2_keywords flag for proc (proc accepts keywords or proc does not accept argument splat)"
    end
    self
  end
  def curry(n = (arity < 0 ? -arity - 1 : arity))
    # A lambda accepts curry(n) only for an n it could actually be called with:
    # at least its required count and, unless it takes a *rest, at most req+opt.
    if lambda?
      a = arity
      req = a < 0 ? -a - 1 : a
      ps = parameters
      has_rest = ps.any? { |p| p[0] == :rest }
      max = req + ps.count { |p| p[0] == :opt }
      if n < req || (!has_rest && n > max)
        raise ArgumentError, "wrong number of arguments (given #{n}, expected #{req})"
      end
    end
    # Always return a curried Proc (arity -1, params [[:rest]]) — even when the
    # required count is already 0 (a variadic/0-arg callee), where the old
    # immediate-call form returned the result instead of a Proc.
    f = self   # capture the callee as a local so it survives instance_exec self-rebind
    lam = lambda?   # CRuby: the curried Proc keeps the callee's lambda-ness
    make = nil
    step = ->(got, more) { all = got + more; all.length >= n ? f.call(*all) : make.call(all) }
    make = ->(got) { __curried(lam ? ->(*more) { step.call(got, more) } : proc { |*more| step.call(got, more) }) }
    make.call([])
  end

  # CRuby's curried Proc is a builtin with no source of its own: it reports
  # [[:rest]] whatever the implementation's parameter is named, has no
  # #source_location, and refuses #binding.
  private def __curried(pr)
    pr.singleton_class.class_eval do
      define_method(:parameters) { |**| [[:rest]] }
      define_method(:source_location) { nil }
      define_method(:binding) { raise ArgumentError, "Can't create Binding from C level Proc" }
    end
    pr
  end
  # Function composition: (f >> g).(x) == g(f(x)); (f << g).(x) == f(g(x)).
  # The result is a lambda iff the first-executed proc is (matches CRuby):
  # for >> that is self, for << that is the argument g.
  # A block given to the composition goes to whichever half runs FIRST (CRuby).
  def >>(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); f = self; lambda? ? ->(*a, &b) { g.call(f.call(*a, &b)) } : proc { |*a, &b| g.call(f.call(*a, &b)) }; end
  def <<(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); f = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a, &b) { f.call(g.call(*a, &b)) } : proc { |*a, &b| f.call(g.call(*a, &b)) }; end
end
class Method
  def >>(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); m = self; ->(*a, &b) { g.call(m.call(*a, &b)) }; end
  def <<(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); m = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a, &b) { m.call(g.call(*a, &b)) } : proc { |*a, &b| m.call(g.call(*a, &b)) }; end
  # Method#curry — curry the method's lambda form.  Uses the *method's* arity
  # (Method#to_proc wraps in a variadic forwarder, so its own arity is unreliable).
  def curry(*args)
    a = arity
    req = a < 0 ? -a - 1 : a
    if args.empty?
      n = req
    else
      n = args[0]
      # n must be at least the required count, and — unless the method takes a
      # rest arg (unbounded) — no more than req+opt (its maximum). CRuby raises
      # ArgumentError otherwise.
      ps = parameters
      has_rest = ps.any? { |p| p[0] == :rest }
      max = req + ps.count { |p| p[0] == :opt }
      if n < req || (!has_rest && n > max)
        raise ArgumentError, "wrong number of arguments (given #{n}, expected #{req})"
      end
    end
    to_proc.curry(n)
  end
end
