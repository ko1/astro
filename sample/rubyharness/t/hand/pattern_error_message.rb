# NoMatchingPatternError は CRuby だと「どうして合わなかったか」を値のあとに付ける:
#   "[1, 2, 3]: [1, 2, 3] length mismatch (given 3, expected 2)"
# koruby は値だけだったので、minitest の assert_pattern のテストが落ちていた。
def try
  yield
rescue NoMatchingPatternError, NoMatchingPatternKeyError => e
  puts "#{e.class}: #{e.message}"
end
try { case [1,2,3]; in [a, b]; end }
try { case [1,2]; in [a, b, c]; end }
try { case {a: 1}; in {b:}; end }
try { case 5; in String; end }
try { case [1,2]; in [1, 3]; end }
try { case {a: 1, b: 2}; in {a: 1, b: 3}; end }
try { case [1,[2,3]]; in [1,[2]]; end }
try { case "x"; in Integer | Float; end }
try { case [1]; in []; end }
try { case nil; in Integer; end }
# 成功する側は今までどおり
case [1,2]; in [a, b]; p [a, b]; end
case {k: 5}; in {k:}; p k; end
case 7; in Integer => n; p n; end
# else があれば raise しない
case [1,2,3]; in [a,b]; p :no; else; p :else_taken; end
