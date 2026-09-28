# undef_method of a Kernel builtin on a class/singleton hides it: send reaches method_missing (mocha)
o = Object.new
def o.method_missing(n, *a) = [:mm, n]
%i[require_relative require load puts p format rand Integer freeze frozen? instance_variables].each do |m|
  sc = o.singleton_class
  sc.send(:undef_method, m) if sc.method_defined?(m) || sc.private_method_defined?(m)
  r = begin; o.__send__(m); rescue Exception => e; e.class; end
  puts "#{m}: #{r.inspect}"
end
class U; undef_method :puts; def go = (puts("x") rescue $!.class); def method_missing(n, *a) = [:mm, n]; end
p U.new.go
class V; undef_method :format; def go = format("%d", 1); end
p((V.new.go rescue $!.class))
