program VariantRecord;
type
  shape = record
    kind: integer;
    case integer of
      0: (radius: real);
      1: (width, height: real);
      2: (sides: integer; side_length: real)
  end;

var
  s: shape;

procedure show(var sh: shape);
begin
  case sh.kind of
    0: writeln('circle r=', sh.radius:0:2);
    1: writeln('rect w=', sh.width:0:2, ' h=', sh.height:0:2);
    2: writeln('poly n=', sh.sides, ' L=', sh.side_length:0:2)
  end
end;

begin
  s.kind   := 0;
  s.radius := 1.5;
  show(s);

  s.kind   := 1;
  s.width  := 3.0;
  s.height := 4.0;
  show(s);

  s.kind   := 2;
  s.sides  := 6;
  s.side_length := 2.5;
  show(s)
end.
