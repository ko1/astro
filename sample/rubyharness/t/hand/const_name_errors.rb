# "wrong constant name" NameErrors carry #name, and const_defined? rejects bad shapes first
m = Module.new
[->{ m.const_get(:"Foo-bar") }, ->{ m.const_set(:"foo", 1) }, ->{ m.const_defined?(:"Foo-bar") },
 ->{ m.const_source_location(:"a-b") }].each do |l|
  begin; l.call; p :noraise; rescue NameError => e; p [e.class, e.name, e.message]; end
end
fm = Module.new.freeze
['', 'foo', ':Foo', 'Foo:Bar', '#$%', 'Foo::Bar', 'Foo::bar', 'Foo', 'A::B::C', 'A::', '::', 'A::::B', '::A', 'Object::String'].each do |s|
  r = begin; fm.const_defined?(s, false); rescue NameError => e; e.message; end
  p [s, r]
end
