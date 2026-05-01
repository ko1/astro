program GcdBench;
{ Compute gcd(a, b) recursively for a wide range of pairs.
  Stresses the recursive function-call path with a 2-argument call. }
const
  N = 2500;
var
  a, b, total: integer;

function gcd(a, b: integer): integer;
begin
  if b = 0 then gcd := a
  else gcd := gcd(b, a mod b)
end;

begin
  total := 0;
  for a := 1 to N do
    for b := 1 to N do
      total := total + gcd(a, b);
  writeln(total)
end.
