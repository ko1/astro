# bare `raise` re-raises $\! (the current exception)
begin
  begin
    raise ArgumentError, "orig"
  rescue
    raise
  end
rescue => e
  p [e.class, e.message]
end
# bare raise with no current exception → RuntimeError ""
begin; raise; rescue => e; p [e.class, e.message]; end
# re-raise preserves the object
err = RuntimeError.new("specific")
begin
  begin; raise err; rescue; raise; end
rescue => e
  p e.equal?(err)
end
