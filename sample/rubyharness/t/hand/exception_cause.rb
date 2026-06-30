begin
  begin
    raise "inner"
  rescue => e
    raise RuntimeError, "outer"
  end
rescue => e2
  p [e2.message, e2.cause&.message]
end
begin
  raise "top-level"
rescue => e
  p e.cause
end
begin
  begin
    begin
      raise "a"
    rescue
      raise "b"
    end
  rescue
    raise "c"
  end
rescue => e
  p [e.message, e.cause.message, e.cause.cause.message]
end
begin
  raise ArgumentError, "x"
rescue => e
  p e.cause.nil?
end
