s = 0; i = 0
while i < 2_000_000
  begin
    raise "boom" if i & 7 == 0
    s += 1
  rescue => e
    s += e.message.length
  end
  i += 1
end
p s
