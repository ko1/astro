program TypeEnumSubrange;
type
  direction = (north, east, south, west);
  small     = 1..10;
  age       = integer;

var
  d: direction;
  s: small;
  a: age;
  i: integer;

procedure show_dir(x: direction);
begin
  case x of
    0: writeln('N');
    1: writeln('E');
    2: writeln('S');
    3: writeln('W')
  end
end;

begin
  d := north; show_dir(d);
  d := west;  show_dir(d);
  for i := 0 to 3 do show_dir(i);

  s := 5;
  writeln('s=', s);

  a := 42;
  writeln('a=', a);

  { enum names are just integer constants }
  writeln(north, ' ', east, ' ', south, ' ', west)
end.
