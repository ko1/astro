program NestedLoopsBench;
{ Pure for-loop overhead: 4-deep nested counter.
  Total iterations = N^4.  Sum is N^4 * (intentional value). }
const
  N = 130;
var
  i, j, k, l, total: integer;
begin
  total := 0;
  for i := 1 to N do
    for j := 1 to N do
      for k := 1 to N do
        for l := 1 to N do
          total := total + 1;
  writeln(total)
end.
