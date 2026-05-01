program StringLit;
var i: integer;
begin
  write('hello');
  write(',');
  write(' ');
  writeln('world');
  for i := 1 to 3 do writeln('x ', i, ' y');
  write('escaped: ');
  writeln('it''s ok');           { single quote escape }
  writeln('width:', 42:6, '|');  { right-aligned in width 6 }
  writeln(7:3, ' ', 12:5)
end.
