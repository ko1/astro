program CharLit;
var c, i: integer;
begin
  c := 'A';
  writeln(c);                     { 65 }
  writeln(chr(c));                { 65 (chr is identity in our int world) }
  for i := 0 to 4 do
    writeln(ord('a') + i);
  if 'a' < 'b' then writeln('order ok')
end.
