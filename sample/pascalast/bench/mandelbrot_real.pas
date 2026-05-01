program MandelbrotReal;
{ Floating-point Mandelbrot — exercises the real arithmetic nodes. }
const
  WIDTH    = 400;
  HEIGHT   = 300;
  MAX_ITER = 500;
var
  x, y, k: integer;
  cx, cy, zx, zy, zx2, zy2: real;
  total: integer;
begin
  total := 0;
  for y := 0 to HEIGHT - 1 do
    for x := 0 to WIDTH - 1 do
    begin
      cx := -2.0 + (3.0 * x) / WIDTH;
      cy := -1.0 + (2.0 * y) / HEIGHT;
      zx := 0.0;
      zy := 0.0;
      zx2 := 0.0;
      zy2 := 0.0;
      k := 0;
      while (zx2 + zy2 < 4.0) and (k < MAX_ITER) do
      begin
        zy := 2.0 * zx * zy + cy;
        zx := zx2 - zy2 + cx;
        zx2 := zx * zx;
        zy2 := zy * zy;
        k := k + 1
      end;
      total := total + k
    end;
  writeln(total)
end.
