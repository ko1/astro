a = nil
1.times { [0, 1] => [a, b] }
p [a, defined?(b)]
x = nil; y = nil
1.times { case {k: 5, j: [1, 2]}; in {k: x, j: [_, y]}; end }
p [x, y]
c = 0
1.times { if 7 in Integer => c then p c end }
p c
r = nil; s = nil
[[1, [2, 3]]].each { |e| e => [r, [*s]] }
p [r, s]
h = nil
1.times { {a: 1, b: 2} => {a:, **h} }
p h
def m
  v = nil
  [1].each { [[9]].each { 3 => v } }
  v
end
p m
z = nil
pr = proc { 4 => z }; pr.call; p z
1.times { [1, 2, 3] => [*, 3 => q] rescue p $! }
$log = []
class D; def initialize(v) = @v = v; def deconstruct = ($log << @v; @v); end
o = D.new([0, 1])
3.times do
  r = case o
      in [1, 2] then :a
      in [*, 5, *] then :b
      in [0, *rest] then rest
      end
  p r
end
p $log.size
$log.clear
inner = D.new([7])
r = case D.new([inner, 2])
    in [[8], _] then 1
    in [x, 2] if (case inner; in [9] then false; in [7] then true; end) then [:g, x.class]
    end
p r, $log.map(&:size)
$log.clear
r = case D.new([1])
    in [2] then 0
    in Integer then 1
    in [1] then 2
    end
p r, $log.size
p((case D.new(:x); in [1] then 1; end rescue $!.class))
