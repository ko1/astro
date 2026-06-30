err = StandardError.new
p err.exception.class
p err.exception.equal?(err)
p err.exception("new msg").message
p err.exception("new msg").class
e2 = RuntimeError.new("orig")
p e2.exception.message
p e2.exception("changed").message
begin; raise ArgumentError, "bad"; rescue => ex; p ex.exception.message; end
begin; raise ArgumentError.new("x").exception("y"); rescue => ex; p [ex.class, ex.message]; end
p StandardError.exception("via class").message
p RuntimeError.exception("rt").class
p ArgumentError.exception.message
