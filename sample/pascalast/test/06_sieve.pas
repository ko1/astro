program Sieve;
const
  N = 100;
var
  i, j, count: integer;
  is_comp: array[2..100] of boolean;

begin
  for i := 2 to N do is_comp[i] := false;
  for i := 2 to N do
    if not is_comp[i] then
    begin
      j := i + i;
      while j <= N do
      begin
        is_comp[j] := true;
        j := j + i
      end
    end;
  count := 0;
  for i := 2 to N do
    if not is_comp[i] then
    begin
      write(i, ' ');
      count := count + 1
    end;
  writeln;
  writeln('count=', count)
end.
