program IncDec;
var
  i, j: integer;
  a: array[0..4] of integer;

begin
  i := 0;
  inc(i);            writeln(i);    { 1 }
  inc(i, 10);        writeln(i);    { 11 }
  dec(i);            writeln(i);    { 10 }
  dec(i, 4);         writeln(i);    { 6 }

  for j := 0 to 4 do a[j] := j * j;
  inc(a[2], 100);
  for j := 0 to 4 do write(a[j], ' ');
  writeln
end.
