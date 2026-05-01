program MandelbrotIntBench;
{ Integer-arithmetic Mandelbrot.  Coordinates and iteration values
  are scaled by SCALE = 2^14 = 16384 so we never need floats.
  Counts the total escape-iteration sum across an WIDTH x HEIGHT
  grid.  Pure compute — no I/O inside the hot loop. }
const
  WIDTH    = 400;
  HEIGHT   = 300;
  MAX_ITER = 500;
  SCALE    = 16384;
  FOUR_S2  = 1073741824;   { 4 * SCALE * SCALE }
var
  x, y, k: integer;
  cx, cy, zx, zy, zx2, zy2: integer;
  total: integer;
begin
  total := 0;
  for y := 0 to HEIGHT - 1 do
    for x := 0 to WIDTH - 1 do
    begin
      { cx in [-2, 1], cy in [-1, 1] (scaled). }
      cx := (x * 3 * SCALE) div WIDTH - 2 * SCALE;
      cy := (y * 2 * SCALE) div HEIGHT - SCALE;
      zx := 0;
      zy := 0;
      k  := 0;
      zx2 := 0;
      zy2 := 0;
      while (zx2 + zy2 < FOUR_S2) and (k < MAX_ITER) do
      begin
        zy := 2 * (zx * zy) div SCALE + cy;
        zx := (zx2 - zy2) div SCALE + cx;
        zx2 := zx * zx;
        zy2 := zy * zy;
        k := k + 1
      end;
      total := total + k
    end;
  writeln(total)
end.
