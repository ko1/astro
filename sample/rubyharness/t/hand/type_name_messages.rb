# "wrong argument type X (expected Module)" names the real class: a Proc is a
# Proc, nil is "nil", a Class is "Class" -- liquid's Strainer asserts on the
# exact text when a filter is not a Module.
class MyThing; end
[Object.new, proc {}, lambda(&:reverse), 1, "s", :sym, nil, true, [], {},
 Class.new, Module.new, MyThing.new, MyThing, 1..2, /re/, 1.5, Struct.new(:x)].each do |x|
  begin
    Class.new { include x }
  rescue TypeError => e
    puts e.message
  else
    puts "#{x.class}: ok"
  end
end
# extend / prepend take the same path
o = Object.new
[proc {}, nil, Class.new].each do |x|
  begin; o.extend(x); rescue TypeError => e; puts "extend: #{e.message}"; end
  begin; Class.new { prepend x }; rescue TypeError => e; puts "prepend: #{e.message}"; end
end
