program ForInPacked;
const N = 5;
var
  a: packed array[1..5] of integer;
  b: array[10..14] of integer;
  i, x, sum: integer;

procedure show(var arr: array[10..14] of integer);
var v: integer;
begin
  for v in arr do write(v, ' ');
  writeln
end;

begin
  for i := 1 to N do a[i] := i * i;
  sum := 0;
  for x in a do sum := sum + x;
  writeln('sum=', sum);             { 1+4+9+16+25 = 55 }

  for i := 10 to 14 do b[i] := 100 + i;
  show(b)
end.
