program SieveBench;
{ Sieve of Eratosthenes — bool array writes + nested loops.
  We sieve up to N a few times so total wall time lands ≥ 1 s.
  N=2_000_000 has 148933 primes; we run it ROUNDS times. }
const
  N      = 2000000;
  ROUNDS = 12;
var
  i, j, count, r, last_count: integer;
  is_comp: array[2..2000000] of boolean;
begin
  last_count := 0;
  for r := 1 to ROUNDS do
  begin
    for i := 2 to N do is_comp[i] := false;
    i := 2;
    while i * i <= N do
    begin
      if not is_comp[i] then
      begin
        j := i * i;
        while j <= N do
        begin
          is_comp[j] := true;
          j := j + i
        end
      end;
      i := i + 1
    end;
    count := 0;
    for i := 2 to N do
      if not is_comp[i] then count := count + 1;
    last_count := count
  end;
  writeln(last_count)
end.
