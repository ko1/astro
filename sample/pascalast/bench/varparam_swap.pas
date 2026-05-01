program VarParamSwap;
{ Stress var-param call: insertion sort using a `swap(var, var)` proc.
  Sorts a 1500-element shuffled array many times. }
const
  N      = 1500;
  ROUNDS = 40;
  SEED0  = 1;
var
  i, r, total: integer;
  seed: integer;
  a: array[1..1500] of integer;

procedure swap(var x, y: integer);
var t: integer;
begin
  t := x; x := y; y := t
end;

procedure isort(n: integer);
var i, j: integer;
begin
  for i := 2 to n do
  begin
    j := i;
    while (j > 1) and (a[j - 1] > a[j]) do
    begin
      swap(a[j - 1], a[j]);
      j := j - 1
    end
  end
end;

function lcg: integer;
begin
  seed := (seed * 1103515245 + 12345) mod 2147483648;
  lcg := seed
end;

begin
  total := 0;
  for r := 1 to ROUNDS do
  begin
    seed := SEED0 + r;
    for i := 1 to N do a[i] := lcg mod 100000;
    isort(N);
    total := total + a[1] + a[N]
  end;
  writeln(total)
end.
