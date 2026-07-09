# Proc#curry — partial application.
class Proc
  def curry(n = (arity < 0 ? -arity - 1 : arity))
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
  def >>(g); f = self; lambda? ? ->(*a) { g.call(f.call(*a)) } : proc { |*a| g.call(f.call(*a)) }; end
  def <<(g); f = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a) { f.call(g.call(*a)) } : proc { |*a| f.call(g.call(*a)) }; end
end
class Method
  def >>(g); m = self; ->(*a) { g.call(m.call(*a)) }; end
  def <<(g); m = self; (g.respond_to?(:lambda?) ? g.lambda? : true) ? ->(*a) { m.call(g.call(*a)) } : proc { |*a| m.call(g.call(*a)) }; end
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
