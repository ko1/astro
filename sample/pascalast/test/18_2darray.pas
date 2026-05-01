program TwoDArray;
const N = 4;
var
  i, j: integer;
  m: array[1..4, 1..4] of integer;
  t: array[1..4] of array[1..4] of integer;
begin
  { fill m with i*10 + j }
  for i := 1 to N do
    for j := 1 to N do m[i, j] := i * 10 + j;

  for i := 1 to N do
  begin
    for j := 1 to N do write(m[i, j]:4);
    writeln
  end;

  { transpose into t }
  for i := 1 to N do
    for j := 1 to N do t[j, i] := m[i, j];

  writeln('--');
  for i := 1 to N do
  begin
    for j := 1 to N do write(t[i, j]:4);
    writeln
  end
end.
