class E < StandardError; end
p E.new(42).to_s
p E.new(42).message
p E.new([1, 2]).to_s
p E.new("hi").message
p E.new.message
p (begin; raise E, 99; rescue => e; e.message; end)
