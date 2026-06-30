xx = 5
p(case 5; in ^xx; "match"; else; "no"; end)
p(case 6; in ^xx; "match"; else; "no"; end)
y = "hello"
p(case "hello"; in ^y; "ystr"; else; "no"; end)
expected = [1, 2]
p(case [1, 2]; in ^expected; "arr"; else; "no"; end)
case [1, 5, 3]; in [_, ^xx, _]; p "nested pin match"; else; p "no"; end
case [1, 6, 3]; in [_, ^xx, _]; p "match"; else; p "nested pin no-match"; end
a = 10
case {val: 10}; in {val: ^a}; p "hash pin"; else; p "no"; end
@iv = 42
case 42; in ^@iv; p "ivar pin"; else; p "no"; end
$gv = 7
case 7; in ^$gv; p "gvar pin"; else; p "no"; end
case 5; in 1..10; p "range match"; else; p "no"; end
case "x"; in String; p "const match"; else; p "no"; end
target = :admin
case {role: :admin}; in {role: ^target}; p "role match"; else; p "no"; end
def check(val, expected); case val; in ^expected; true; else; false; end; end
p check(5, 5)
p check(5, 6)
p check("a", "a")
first = 1
case [1, 2, 3]; in [^first, *rest]; p [:pin_first, rest]; else; p :no; end
