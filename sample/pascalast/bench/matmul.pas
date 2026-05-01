program MatMulBench;
{ Square integer matrix multiply: C = A * B.  N=128 → 2*N^3 ≈ 4.2 M
  multiply-adds per round.  We keep matrices in 1D arrays indexed as
  i*N + j (Pascal arrays support arbitrary low bound, but row-major
  flat is the simplest portable layout). }
const
  N      = 128;
  NSQ    = 16384;       { N*N }
  ROUNDS = 10;
var
  i, j, k, r, s: integer;
  a: array[0..16383] of integer;
  b: array[0..16383] of integer;
  c: array[0..16383] of integer;

begin
  for i := 0 to N - 1 do
    for j := 0 to N - 1 do
    begin
      a[i * N + j] := i + j;
      b[i * N + j] := i - j
    end;

  s := 0;
  for r := 1 to ROUNDS do
  begin
    for i := 0 to N - 1 do
      for j := 0 to N - 1 do
      begin
        k := 0;
        c[i * N + j] := 0;
        while k < N do
        begin
          c[i * N + j] := c[i * N + j] + a[i * N + k] * b[k * N + j];
          k := k + 1
        end
      end;
    s := s + c[0] + c[NSQ - 1]
  end;
  writeln(s)
end.
