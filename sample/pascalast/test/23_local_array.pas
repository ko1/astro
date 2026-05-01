program LocalArray;

procedure work;
var
  a: array[1..5] of integer;
  i, sum: integer;
begin
  for i := 1 to 5 do a[i] := i * i;
  sum := 0;
  for i := 1 to 5 do sum := sum + a[i];
  writeln('sum=', sum);
  for i := 1 to 5 do write(a[i], ' ');
  writeln
end;

procedure work_neg;
var
  b: array[-2..2] of integer;
  i: integer;
begin
  for i := -2 to 2 do b[i] := i * 10;
  for i := -2 to 2 do write(b[i], ' ');
  writeln
end;

begin
  work;
  work_neg
end.
