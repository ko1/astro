program VarParam;
var
  a, b: integer;
  arr: array[1..5] of integer;
  i: integer;

procedure swap(var x, y: integer);
var t: integer;
begin
  t := x; x := y; y := t
end;

procedure incr(var x: integer; n: integer);
begin
  x := x + n
end;

procedure swap_chain(var p, q: integer);
begin
  swap(p, q)            { var-arg pass-through }
end;

begin
  a := 10; b := 20;
  swap(a, b);
  writeln(a, ' ', b);                    { 20 10 }

  incr(a, 5);
  writeln(a);                            { 25 }

  for i := 1 to 5 do arr[i] := 6 - i;    { 5 4 3 2 1 }
  swap(arr[1], arr[5]);
  for i := 1 to 5 do write(arr[i], ' '); writeln;

  swap_chain(arr[2], arr[4]);            { pass-through across two levels }
  for i := 1 to 5 do write(arr[i], ' '); writeln
end.
