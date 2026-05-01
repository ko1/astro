program Arith;
var
  a, b: integer;
begin
  a := 12;
  b := 5;
  writeln(a + b);
  writeln(a - b);
  writeln(a * b);
  writeln(a div b);
  writeln(a mod b);
  writeln(-a);
  writeln(abs(-7));
  writeln(sqr(9));
  writeln(succ(3), ' ', pred(3));
  writeln((1 + 2) * (3 + 4))
end.
