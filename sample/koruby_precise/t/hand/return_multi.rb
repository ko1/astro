def two; return 1, 2; end
p two

def three(x); return x, x*2, x*3; end
p three(5)

def find(arg)
  [[:a, 10], [:b, 20]].each do |id, v|
    return id, v if id.to_s == arg
  end
  nil
end
p find("b")

# next with multiple values inside a block → [a, b]
r = [1, 2, 3].map do |x|
  next x, x * 10
end
p r
