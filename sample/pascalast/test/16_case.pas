program CaseTest;
var i, k: integer;

procedure classify(n: integer);
begin
  case n of
    0:        writeln('zero');
    1, 2, 3:  writeln('small ', n);
    4..10:    writeln('mid ', n);
    -5..-1:   writeln('neg ', n)
  else
    writeln('other ', n)
  end
end;

begin
  for i := -6 to 12 do classify(i);

  { case with computed expression }
  for k := 1 to 5 do
    case k * k of
      1, 4: writeln('square small');
      9..25: writeln('square big')
    else
      writeln('?')
    end
end.
