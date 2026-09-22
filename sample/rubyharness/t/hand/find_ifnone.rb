h = { a: 1, b: 2 }
p h.find { |k, v| v == 2 }
p h.find(proc { :none }) { |k, v| v == 9 }
p h.detect(proc { :none2 }) { |k, v| false }
p h.find { |k, v| false }
p (1..5).find(proc { :r }) { |i| i > 9 }
p (1..5).find { |i| i > 3 }
p [1,2].find(proc { :a }) { false }
p ["x"].each_with_index.find(proc { :b }) { false } rescue p $!.class
e = h.find(proc { :c })
p e.class
p e.each { |k, v| v == 2 }
def f
  h = { a: 1 }
  h.find(Proc.new { return :returned }) { |k, _| k == :zz }
  :not_reached
end
p f
p ({a: 1}.find(proc { :d }) { |k, v| k == :a })
