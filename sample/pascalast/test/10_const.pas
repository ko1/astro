program ConstTest;
const
  N    = 5;
  ZERO = 0;
  NEG  = -7;
var
  i: integer;
  acc: integer;
begin
  acc := ZERO;
  for i := 1 to N do acc := acc + i;
  writeln(acc);
  writeln(NEG);
  writeln(N * N)
end.
