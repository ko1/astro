# Object#initialize_copy: frozen receiver (incl. immediates) → FrozenError,
# different class → TypeError. vs ruby.
begin; 1.send(:initialize_copy, Object.new); rescue => e; p e.class; end
begin; :s.send(:initialize_copy, Object.new); rescue => e; p e.class; end
o = Object.new.freeze
begin; o.send(:initialize_copy, Object.new); rescue => e; p e.class; end
k = Class.new; s = Class.new(k)
begin; k.new.send(:initialize_copy, s.new); rescue => e; p e.class; end
begin; k.new.send(:initialize_copy, 1); rescue => e; p e.class; end
x = Object.new
p x.send(:initialize_copy, x).equal?(x)
