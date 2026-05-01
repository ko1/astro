program LeibnizPi;
{ Leibniz series for pi — pure real arithmetic in a tight loop.
  N=200_000_000 keeps total wall time near 1 s. }
const
  N = 40000000;
var
  i: integer;
  s, t, sign: real;
begin
  s := 0.0;
  sign := 1.0;
  for i := 0 to N - 1 do
  begin
    t := 1.0 / (2 * i + 1);
    s := s + sign * t;
    sign := -sign
  end;
  writeln(4.0 * s : 0 : 6)
end.
