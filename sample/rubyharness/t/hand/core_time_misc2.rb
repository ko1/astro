# Time: deconstruct_keys type check, private _dump/_load (Marshal still works),
# stable #hash, <=> with an Integer, + with a #to_r Numeric.
d = Time.utc(2022, 10, 5, 13, 30)
[1, "asd", :x, {}].each do |k|
  begin
    d.deconstruct_keys(k)
  rescue TypeError => e
    puts e.message
  end
end
p d.deconstruct_keys([:year, :zone])
p Time.private_instance_methods(false).include?(:_dump)
p Time.private_methods(false).include?(:_load)
p Marshal.load(Marshal.dump(d)) == d
p Time.at(1234).hash == Time.at(1234).hash
p Time.at(1234).hash == Time.at(1235).hash
p Time.at(1234).eql?(Time.at(1234)), { Time.at(5) => 1 }[Time.at(5)]
t = Time.at(1)
p(t <=> 2)
p(2 <=> t)
p(t <=> nil)
p(t <=> "x")

class N < Numeric
  def to_r = Rational(10)
end
p Time.at(100) + N.new == Time.at(110)
p Time.at(100) - N.new == Time.at(90)
begin
  Time.at(100) + Object.new
rescue TypeError => e
  puts e.message
end
