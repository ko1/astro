# PP.pp は out に << するだけ (String でもよい)。temple のエラーメッセージが
# String を渡す。
require "pp"
s = "".dup
PP.pp([:unknown], s)
p s
t = "head\n".dup
PP.pp({a: 1}, t)
p t
u = "".dup
PP.singleline_pp([1, 2], u)
p u
p [1, 2].pretty_inspect
