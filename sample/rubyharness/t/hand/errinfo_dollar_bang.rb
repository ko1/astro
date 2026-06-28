begin
  raise ArgumentError, "boom"
rescue
  p $!.class
  p $!.message
end
p $!                 # cleared after handling (top-level outer was nil)

x = (raise("modifier") rescue $!.message)
p x
p $!                 # nil again

# nested: outer $! restored after inner rescue
begin
  raise "outer"
rescue => e1
  before = $!.message
  begin
    raise "inner"
  rescue
    p $!.message      # "inner"
  end
  after = $!.message  # restored to "outer"
  p [before, after]
end
p $!                 # nil
