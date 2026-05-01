program CollatzBench;
{ For each k in [1..N], compute the Collatz stopping time and accumulate.
  Pure tight integer loop — div/mod on the hot path. }
const
  N = 400000;
var
  k, m, steps: integer;
  total: integer;
begin
  total := 0;
  for k := 1 to N do
  begin
    m := k;
    steps := 0;
    while m > 1 do
    begin
      if (m mod 2) = 0 then m := m div 2
      else m := 3 * m + 1;
      steps := steps + 1
    end;
    total := total + steps
  end;
  writeln(total)
end.
