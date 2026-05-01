program ProcsAndFuncs;
var
  i: integer;

procedure greet(n: integer);
var
  k: integer;
begin
  for k := 1 to n do writeln('hi ', k)
end;

function square(x: integer): integer;
begin
  square := x * x
end;

function add(a, b: integer): integer;
begin
  add := a + b
end;

function fact(n: integer): integer;
begin
  if n <= 1 then fact := 1
  else fact := n * fact(n - 1)
end;

function fib(n: integer): integer;
begin
  if n < 2 then fib := n
  else fib := fib(n - 1) + fib(n - 2)
end;

function ack(m, n: integer): integer;
begin
  if m = 0 then ack := n + 1
  else if n = 0 then ack := ack(m - 1, 1)
  else ack := ack(m - 1, ack(m, n - 1))
end;

begin
  greet(3);
  writeln(square(7));
  writeln(add(20, 22));
  writeln(fact(10));
  writeln(fib(15));
  writeln(ack(3, 4));
  for i := 1 to 5 do writeln(square(i))
end.
