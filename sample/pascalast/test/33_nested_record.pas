program NestedRecord;
type
  point = record x, y: integer end;
  rect  = record tl, br: point end;
  thing = record id: integer; pos: point; size: point end;

var
  r: rect;
  t: thing;

procedure print_pt(var p: point);
begin writeln('(', p.x, ',', p.y, ')') end;

begin
  r.tl.x := 1; r.tl.y := 2;
  r.br.x := 9; r.br.y := 8;
  print_pt(r.tl);
  print_pt(r.br);
  writeln('w=', r.br.x - r.tl.x, ' h=', r.br.y - r.tl.y);

  t.id      := 42;
  t.pos.x   := 10; t.pos.y := 20;
  t.size.x  := 100; t.size.y := 50;
  writeln('id=', t.id, ' pos=(', t.pos.x, ',', t.pos.y, ') size=(', t.size.x, ',', t.size.y, ')')
end.
