# a class's own #eval gets only the arguments written (the hidden binding is for Kernel#eval)
x = 5
p eval("x + 1"), eval("__FILE__"), eval("__LINE__"), eval("[__FILE__, __LINE__]", nil, "f.rb", 7)
begin; eval("raise 'boom'"); rescue => e; p e.backtrace.first(2); end
begin; eval("1 +"); rescue SyntaxError => e; p e.message.lines.first.chomp; end
def m = eval("__method__"); p m
class Net2; def eval(v) = [:mine, v]; def go = eval(3); def go2 = eval(4, nil); end
p Net2.new.go, (Net2.new.go2 rescue $!.class)
p binding.local_variable_get(:x), eval("local_variables").include?(:x)
