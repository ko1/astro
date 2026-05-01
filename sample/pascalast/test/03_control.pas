program Control;
var
  i, sum: integer;
begin
  sum := 0;
  for i := 1 to 10 do
    sum := sum + i;
  writeln('sum1to10=', sum);

  sum := 0;
  for i := 10 downto 1 do
    sum := sum - i;
  writeln('downto=', sum);

  i := 0;
  while i < 5 do
  begin
    write(i, ' ');
    i := i + 1
  end;
  writeln;

  i := 5;
  repeat
    write(i, ' ');
    i := i - 1
  until i = 0;
  writeln;

  if 10 > 5 then writeln('yes') else writeln('no');
  if 1 = 2 then writeln('bad')
  else if 1 = 1 then writeln('one')
  else writeln('also bad')
end.
