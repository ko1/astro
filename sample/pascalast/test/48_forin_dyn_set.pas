program ForInDynSet;
{ for-in over dynamic arrays and sets — round 8 extension. }
var
  d: array of integer;
  s: set of byte;
  x: integer;
  total: integer;
begin
  setlength(d, 5);
  d[0] := 10; d[1] := 20; d[2] := 30; d[3] := 40; d[4] := 50;
  total := 0;
  for x in d do total := total + x;
  writeln('dyn sum=', total);

  s := [3, 7, 11, 13, 31];
  total := 0;
  write('set:');
  for x in s do
  begin
    write(' ', x);
    total := total + x
  end;
  writeln;
  writeln('set sum=', total)
end.
