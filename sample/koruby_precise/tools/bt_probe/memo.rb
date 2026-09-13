module M; def f; raise "x"; end; end
class C; include M; end
begin; C.new.f; rescue => e; puts e.backtrace[0]; end
module M; module_function :f; end
begin; M.f; rescue => e; puts e.backtrace[0]; end
class C; alias g f; end
begin; C.new.g; rescue => e; puts e.backtrace[0]; end
