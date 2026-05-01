program Arrays;
var
  i, j, n: integer;
  a: array[1..10] of integer;

procedure print_array(n: integer);
var
  k: integer;
begin
  for k := 1 to n do write(a[k], ' ');
  writeln
end;

begin
  n := 10;
  for i := 1 to n do a[i] := (n - i) + 1;   { 10 9 8 ... 1 }
  print_array(n);

  { simple insertion sort }
  for i := 2 to n do
  begin
    j := i;
    while (j > 1) and (a[j - 1] > a[j]) do
    begin
      { swap a[j-1], a[j] without a temporary }
      a[j - 1] := a[j - 1] + a[j];
      a[j]     := a[j - 1] - a[j];
      a[j - 1] := a[j - 1] - a[j];
      j := j - 1
    end
  end;
  print_array(n)
end.
