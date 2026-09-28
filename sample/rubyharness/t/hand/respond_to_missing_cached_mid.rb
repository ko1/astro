class X; def respond_to_missing?(n, p = false) = n.to_s.start_with?("dyn_"); def method_missing(n, *a) = n.to_s.start_with?("dyn_") ? [:mm, n] : super; end
x = X.new
p x.respond_to?(:dyn_a), x.respond_to?(:zzz), x.respond_to?("dyn_b"), 1.respond_to?(:foo), x.method(:dyn_q).call, (x.zzz rescue $!.class)
p x.respond_to?(:dyn_c, true), Object.new.respond_to?(:puts), Object.new.respond_to?(:puts, true)
