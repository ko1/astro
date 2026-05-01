program UsesTest;
uses myunit;

var i: integer;
begin
  greet;
  for i := 1 to 5 do writeln(i, ' -> ', square(i))
end.
