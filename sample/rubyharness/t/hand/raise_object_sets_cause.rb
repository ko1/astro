# `raise exc_object` inside a rescue takes $! as its cause unless it already has one (also on a re-raise).
def t
  yield
rescue => e
  [e.class, e.message, e.cause&.message]
end
p t { begin; raise "orig"; rescue; raise RuntimeError.new("wrapped"); end }
p t { begin; raise "orig"; rescue; raise RuntimeError, "cls"; end }
p t { begin; raise "orig"; rescue; raise "str"; end }
p t { begin; raise "orig"; rescue; e = RuntimeError.new("pre"); raise e; end }
p t { begin; raise "orig"; rescue; raise RuntimeError.new("expl"), cause: nil; end }
p t { begin; raise "orig"; rescue => o; raise o; end }
p t { begin; raise "orig"; rescue; x = (raise ArgumentError, "inner" rescue $!); raise x; end }
p t { raise RuntimeError.new("nocause") }
ex = (begin; begin; raise "a"; rescue; raise TypeError.new("b"); end; rescue => e; e; end)
p ex.cause.class
first = (raise "first" rescue $!)
p t { begin; raise "other"; rescue; raise first; end }
fresh = RuntimeError.new("fresh")
p t { begin; raise "o2"; rescue; begin; raise "o3"; rescue; raise fresh; end; end }
class MyE < StandardError; def self.exception(*) = new("via exception"); end
p t { begin; raise "o4"; rescue; raise MyE; end }
