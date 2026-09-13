module M; def f; raise "x"; end; end
module M; module_function :f; end
begin; M.f; rescue => e; puts e.backtrace[0]; end
class C; def f; raise "x"; end; alias g f; end
begin; C.new.f; rescue => e; puts e.backtrace; end
puts "--"
begin; C.new.g; rescue => e; puts e.backtrace; end
puts "--"
def h; raise "y"; end
alias hh h
begin; hh; rescue => e; puts e.backtrace; end
