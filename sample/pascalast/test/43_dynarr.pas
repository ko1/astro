program DynArr;
var
  a: array of integer;
  b: array of integer;
  i, sum: integer;
begin
  setlength(a, 5);
  for i := 0 to length(a) - 1 do a[i] := (i + 1) * (i + 1);
  for i := 0 to length(a) - 1 do write(a[i], ' ');
  writeln;
  writeln('len=', length(a));

  { grow }
  setlength(a, 8);
  for i := 5 to 7 do a[i] := i * 100;
  for i := 0 to length(a) - 1 do write(a[i], ' ');
  writeln;

  { sum }
  sum := 0;
  for i := 0 to length(a) - 1 do sum := sum + a[i];
  writeln('sum=', sum);

  { shrink }
  setlength(a, 3);
  writeln('after shrink len=', length(a));
  for i := 0 to length(a) - 1 do write(a[i], ' ');
  writeln;

  { empty array }
  writeln('b empty length=', length(b))
end.
