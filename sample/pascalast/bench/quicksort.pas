program QuickSortBench;
{ Repeated quicksort over a 4096-element array.
  Each round re-fills the array with a deterministic LCG sequence
  (so the sort actually does work each time).  Sums all entries
  after sorting and prints the running total — a result that
  depends on the data so the optimizer can't elide the work. }
const
  N      = 4096;
  ROUNDS = 500;
var
  i, r: integer;
  seed: integer;
  total: integer;
  a: array[1..4096] of integer;

procedure swap(i, j: integer);
var t: integer;
begin
  t := a[i]; a[i] := a[j]; a[j] := t
end;

procedure qsort(lo, hi: integer);
var
  pivot, i, j: integer;
begin
  if lo < hi then
  begin
    pivot := a[hi];
    i := lo - 1;
    for j := lo to hi - 1 do
      if a[j] <= pivot then
      begin
        i := i + 1;
        swap(i, j)
      end;
    swap(i + 1, hi);
    qsort(lo, i);
    qsort(i + 2, hi)
  end
end;

function lcg: integer;
begin
  seed := (seed * 1103515245 + 12345) mod 2147483648;
  lcg  := seed
end;

begin
  total := 0;
  seed := 1;
  for r := 1 to ROUNDS do
  begin
    for i := 1 to N do a[i] := lcg mod 100000;
    qsort(1, N);
    total := total + a[1] + a[N]
  end;
  writeln(total)
end.
