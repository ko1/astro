program StringMut;
var s, t: string;
    i: integer;
begin
  s := 'hello world';
  writeln(s);

  for i := 1 to length(s) do
    if (s[i] >= ord('a')) and (s[i] <= ord('z')) then
      s[i] := s[i] - ord('a') + ord('A');
  writeln(s);

  { confirm the literal 'hello world' isn't clobbered: t derived from
    a fresh literal }
  t := 'hello world';
  writeln(t)
end.
