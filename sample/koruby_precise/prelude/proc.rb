# Proc#curry — partial application.
class Proc
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
      # a fixed-arity method must be curried at exactly its arity; a variadic one
      # at no fewer than its required count (CRuby raises otherwise).
      if (a >= 0 && n != a) || (a < 0 && n < req)
        raise ArgumentError, "wrong number of arguments (given #{n}, expected #{req})"
      end
    end
    to_proc.curry(n)
  end
end
