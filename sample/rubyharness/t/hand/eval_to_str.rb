# Kernel#eval coerces a non-String source via #to_str. vs ruby.
class ToStr; def to_str; "1 + 2"; end; end
p eval(ToStr.new)
p eval("3 * 4")
begin; eval(42); rescue TypeError; p :type; end
