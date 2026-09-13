module M; def f; raise "x"; end; end
class C; include M; alias g f; end
begin; C.new.g; rescue => e; puts e.backtrace; end
puts "--"
class D < C; end
begin; D.new.g; rescue => e; puts e.backtrace; end
puts "-- caller"
module N; def k; caller(0); end; end
class E; include N; alias kk k; end
puts E.new.kk
