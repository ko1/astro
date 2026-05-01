program QuickSort;
const
  N = 20;
var
  i: integer;
  a: array[1..20] of integer;

procedure swap(i, j: integer);
var t: integer;
begin
  t := a[i];
  a[i] := a[j];
  a[j] := t
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

begin
  { reverse-sorted input }
  for i := 1 to N do a[i] := N - i + 1;
  qsort(1, N);
  for i := 1 to N do write(a[i], ' ');
  writeln
end.
