program OpenArrayParam;
{ open-array param: `array of T` (no bounds) — value-passed dynarr. }

function sum_oarr(a: array of integer): integer;
var i: integer;
    s: integer;
begin
  s := 0;
  for i := low(a) to high(a) do
    s := s + a[i];
  sum_oarr := s
end;

procedure double_oarr(a: array of integer);
var i: integer;
begin
  for i := 0 to length(a) - 1 do
    a[i] := a[i] * 2;
end;

var
  d: array of integer;
  i: integer;
begin
  setlength(d, 5);
  for i := 0 to 4 do d[i] := i + 1;

  writeln('low=', low(d), ' high=', high(d), ' length=', length(d));
  writeln('sum=', sum_oarr(d));

  double_oarr(d);
  write('doubled:');
  for i := 0 to high(d) do write(' ', d[i]);
  writeln
end.
