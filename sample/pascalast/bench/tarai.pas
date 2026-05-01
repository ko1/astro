program TaraiBench;
{ Takeuchi function — extreme recursion stress.
  tarai(12, 6, 0) = 12.  Ratio of inner calls to outer is huge. }
function tarai(x, y, z: integer): integer;
begin
  if x <= y then tarai := y
  else tarai := tarai(tarai(x - 1, y, z),
                      tarai(y - 1, z, x),
                      tarai(z - 1, x, y))
end;
begin
  writeln(tarai(13, 6, 0))
end.
