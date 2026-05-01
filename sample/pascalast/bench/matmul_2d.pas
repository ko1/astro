program MatMulBench2D;
{ Square integer matmul, 2D array form.  Same N as matmul.pas but
  uses native 2D indexing (a[i, j]) instead of flat. }
const
  N      = 128;
  ROUNDS = 25;
var
  i, j, k, r, s: integer;
  a: array[0..127, 0..127] of integer;
  b: array[0..127, 0..127] of integer;
  c: array[0..127, 0..127] of integer;
begin
  for i := 0 to N - 1 do
    for j := 0 to N - 1 do
    begin
      a[i, j] := i + j;
      b[i, j] := i - j
    end;

  s := 0;
  for r := 1 to ROUNDS do
  begin
    for i := 0 to N - 1 do
      for j := 0 to N - 1 do
      begin
        c[i, j] := 0;
        for k := 0 to N - 1 do
          c[i, j] := c[i, j] + a[i, k] * b[k, j]
      end;
    s := s + c[0, 0] + c[N - 1, N - 1]
  end;
  writeln(s)
end.
