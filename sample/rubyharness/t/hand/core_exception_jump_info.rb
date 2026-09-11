# LocalJumpError#reason / #exit_value for an orphan return; StopIteration#result.
def get_me_a_return
  Proc.new { return 42 }
end
begin
  get_me_a_return.call
rescue LocalJumpError => e
  p e.reason, e.exit_value, e.message
end

obj = Object.new
def obj.each
  yield :yield_returned_1
  yield :yield_returned_2
  :method_returned
end
enum = obj.to_enum
p enum.next, enum.next
begin
  enum.next
rescue StopIteration => e
  p e.result, e.message
end
begin
  enum.next
rescue StopIteration => e
  p e.result
end
e2 = [1, 2].each
p e2.next, e2.next
p((e2.next rescue :stop))
p StopIteration.new("x").result
