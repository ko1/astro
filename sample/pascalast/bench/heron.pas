program HeronBench;
{ Integer square root by Heron's iteration, run for many inputs.
  Tight loop with div + comparison; no array, no call.
  Pascal lacks `break`, so we control the loop with a flag. }
const
  ROUNDS = 20;
  HI     = 200000;
var
  k, r: integer;
  x, prev, total: integer;
  done: boolean;
begin
  total := 0;
  for r := 1 to ROUNDS do
    for k := 1 to HI do
    begin
      x := k;
      done := false;
      while not done do
      begin
        prev := x;
        x := (x + k div x) div 2;
        if x >= prev then
        begin
          total := total + prev;
          done  := true
        end
      end
    end;
  writeln(total)
end.
