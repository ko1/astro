program Nested;
{ Two-level nested loops with early-ish termination via boolean. }
var
  i, j, hits: integer;
  found: boolean;
begin
  hits := 0;
  for i := 1 to 6 do
  begin
    found := false;
    for j := 1 to 6 do
      if (i + j) = 7 then
      begin
        write('(', i, ',', j, ') ');
        hits := hits + 1;
        found := true
      end;
    if found then writeln('done', i)
  end;
  writeln('hits=', hits)
end.
