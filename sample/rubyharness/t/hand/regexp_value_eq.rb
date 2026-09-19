# Array#include? / Hash のキー照合は korb_value_eq を通るが、そこに Regexp が
# 無かったので同じパターンの Regexp 同士が別物になっていた。
# (minitest の register_spec_type が `[[//, Minitest::Spec]].include?([//, …])` を使う)
p(// == //)
p(/abc/ == /abc/)
p(/abc/i == /abc/i)
p(/abc/i == /abc/)
p(/abc/m == /abc/)
p [//].include?(//)
p [[//, String]].include?([//, String])
p({ // => 1 }[//])
p({ /a/i => 2 }[/a/i])
p({ /a/i => 2 }[/a/])
p [/a/, /b/] == [/a/, /b/]
p [/a/, /b/] == [/a/, /c/]
p(//.eql?(//))
p(//.hash == //.hash)
p [/a/, /a/].uniq.size
p([/a/] - [/a/])
p({ /x/ => 1 }.key?(/x/))
p [/a/, /b/].index(/b/)
