# Kernel#fail — alias of raise
begin; fail "boom"; rescue => e; p [e.class, e.message]; end
begin; fail ArgumentError, "bad"; rescue => e; p [e.class, e.message]; end
begin
  begin; raise IndexError, "orig"; rescue; fail; end   # bare fail re-raises $\!
rescue => e
  p [e.class, e.message]
end
