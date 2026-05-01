program RealTest;
const PI = 3.14159265358979;
var
  x, y, z: real;
  i: integer;

function poly(t: real): real;
begin
  poly := 1.0 + t + t*t / 2.0 + t*t*t / 6.0
end;

function sumi(n: integer): real;
var k: integer; s: real;
begin
  s := 0.0;
  for k := 1 to n do s := s + 1.0 / k;
  sumi := s
end;

begin
  x := 1.5;
  y := 2.5;
  z := x + y;
  writeln(z:0:4);

  z := x * y - 0.25;
  writeln(z:0:4);

  z := sqrt(2.0);
  writeln(z:0:6);

  writeln(PI:0:6);

  { mixed int/real promotion }
  i := 7;
  z := i + 0.5;
  writeln(z:0:4);

  { integer / integer = real }
  z := 5 / 2;
  writeln(z:0:4);

  { trunc / round }
  writeln(trunc(3.9));
  writeln(round(3.5));
  writeln(round(-3.5));

  { function returning real }
  writeln(poly(0.5):0:6);
  writeln(sumi(10):0:6);

  { abs(real) }
  writeln(abs(-7.25):0:4)
end.
