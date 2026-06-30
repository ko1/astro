p((-"interned").frozen?)
p((-"interned") == "interned")
p((+"mutable").frozen?)
s = "hi".freeze
p((+s).frozen?)
p((+s) == "hi")
p((-"x").frozen?)
fs = -"frozen"
p fs.frozen?
ms = +fs
p ms.frozen?
p ms == "frozen"
p((-"abc") == "abc")
str = "data"
p((-str).frozen?)
p str.frozen?
p [-"a", -"b"].all?(&:frozen?)
p((+"copy").frozen?)
x = "y".freeze
p((+x).equal?(x))
p((-x).equal?(x))
