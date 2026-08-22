# Proc#curry — partial application.
class Proc
  def to_proc; self; end
  # Mark this Proc so a bare kwargs Hash flows through *args (koruby already
  # threads a trailing kwargs Hash through splat, so this is a no-op that returns self).
  def ruby2_keywords; self; end
  def curry(n = (arity < 0 ? -arity - 1 : arity))
    # A lambda must be curried at exactly its required arity (or, for a variadic
    # lambda, at least its required count); CRuby raises ArgumentError otherwise.
    if lambda?
      a = arity
      if a >= 0 && n != a
        raise ArgumentError, "wrong number of arguments (given #{n}, expected #{a})"
      elsif a < 0 && n < (-a - 1)
        raise ArgumentError, "wrong number of arguments (given #{n}, expected #{-a - 1}+)"
      end
    end
    # Always return a curried Proc (arity -1, params [[:rest]]) — even when the
    # required count is already 0 (a variadic/0-arg callee), where the old
    # immediate-call form returned the result instead of a Proc.
    f = self   # capture the callee as a local so it survives instance_exec self-rebind
    make = nil
    make = ->(got) { ->(*more) { all = got + more; all.length >= n ? f.call(*all) : make.call(all) } }
    make.call([])
  end
  # Function composition: (f >> g).(x) == g(f(x)); (f << g).(x) == f(g(x)).
  # The result is a lambda iff the first-executed proc is (matches CRuby):
  # for >> that is self, for << that is the argument g.
  # NOTE: no `&b` block-forwarding into the composed calls — forwarding a captured
  # block-arg through a nested Proc#call is a pre-existing GC-unsafe path that
  # SEGVs (see [[project_koruby_precise_cproc_block]] escaped-proc &proc gap).
  def >>(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); f = self; lambda? ? ->(*a) { g.call(f.call(*a)) } : proc { |*a| g.call(f.call(*a)) }; end
  def <<(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); f = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a) { f.call(g.call(*a)) } : proc { |*a| f.call(g.call(*a)) }; end
end
class Method
  def >>(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); m = self; ->(*a) { g.call(m.call(*a)) }; end
  def <<(g); raise TypeError, "callable object is expected" unless g.respond_to?(:call); m = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a) { m.call(g.call(*a)) } : proc { |*a| m.call(g.call(*a)) }; end
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

class Proc
  def ruby2_keywords
    self   # kwargs already flow through the Hash flag; marking is a no-op
  end
end
