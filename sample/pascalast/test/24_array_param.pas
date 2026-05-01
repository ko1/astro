program ArrayParam;
var g: array[1..6] of integer;
    i: integer;

procedure fill(var a: array[1..6] of integer; v: integer);
var k: integer;
begin
  for k := 1 to 6 do a[k] := v
end;

function sum_arr(var a: array[1..6] of integer): integer;
var k, s: integer;
begin
  s := 0;
  for k := 1 to 6 do s := s + a[k];
  sum_arr := s
end;

procedure scale(var a: array[1..6] of integer; factor: integer);
var k: integer;
begin
  for k := 1 to 6 do a[k] := a[k] * factor
end;

begin
  fill(g, 7);
  writeln('sum=', sum_arr(g));        { 42 }
  scale(g, 2);
  for i := 1 to 6 do write(g[i], ' '); writeln
end.
