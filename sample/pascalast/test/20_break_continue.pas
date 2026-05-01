program BreakCont;
var i, sum, found: integer;

procedure stop_at_5;
var j: integer;
begin
  j := 0;
  while j < 100 do
  begin
    if j = 5 then break;
    j := j + 1
  end;
  writeln('stopped at ', j)
end;

function find_first_div(n, lim: integer): integer;
var k: integer;
begin
  for k := 2 to lim do
    if (n mod k) = 0 then
    begin
      Result := k;
      exit
    end;
  Result := -1
end;

begin
  stop_at_5;

  sum := 0;
  for i := 1 to 20 do
  begin
    if (i mod 2) = 0 then continue;
    sum := sum + i
  end;
  writeln('odd sum 1..20 = ', sum);

  writeln(find_first_div(91, 20));   { 7  (91 = 7*13) }
  writeln(find_first_div(13, 20));   { 13 (prime, 13 itself in range) }

  { exit with value }
  found := find_first_div(100, 50);
  writeln(found)
end.
