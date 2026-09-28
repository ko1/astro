m = Module.new
m.const_set(:Ω, 1); p m::Ω, m.const_get(:Ω), m.const_defined?(:Màxim)
m.autoload(:Màxim, "/nonexistent/x.rb"); p m.autoload?(:Màxim)
p((begin; m.const_set(:ω, 1); rescue => e; e.class; end), (begin; m.const_set(:"Ω x", 1); rescue => e; e.class; end))
Ωmega = 5
p Ωmega, Object.const_defined?(:Ωmega), m.const_defined?("Ω")
