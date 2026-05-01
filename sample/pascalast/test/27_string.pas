program StringTest;
var
  s, t, u: string;
  i: integer;
begin
  s := 'hello';
  t := 'world';
  u := s + ', ' + t + '!';
  writeln(u);
  writeln('length(u) = ', length(u));

  if s = 'hello' then writeln('eq ok');
  if s < t then writeln('order ok');

  for i := 1 to length(s) do write(chr(s[i]));
  writeln;

  u := '';
  for i := 1 to 5 do u := u + 'ab';
  writeln(u);
  writeln('length=', length(u))
end.
