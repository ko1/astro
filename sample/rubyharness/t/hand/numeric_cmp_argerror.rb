# Numeric/String/Symbol </<=/>/>= with an incomparable operand → ArgumentError
[[1.0, "x"], [1, "x"], [1.0, nil], [3, []], [2.5, :sym], [10**40, "y"], [(1r/2), nil]].each do |a, b|
  %w[< > <= >=].each do |op|
    r = begin; a.send(op, b); rescue => e; e.class.to_s; end
    print "#{r} "
  end
  puts
end
# normal comparisons still work
p 1.0 < 2
p 2 <= 2.0
p((1r/2) < 1)
p 10**40 > 5
