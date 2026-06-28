class C; attr_accessor :x; end
o = C.new
o.freeze
p (begin; o.instance_variable_set(:@y, 1); rescue FrozenError; "FE"; end)
p (begin; o.x = 5; rescue FrozenError; "FE"; end)
p o.frozen?
