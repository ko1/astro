program Records;
type
  point = record x, y: integer end;
  rect  = record left, top, right, bottom: integer end;

var
  p, q: point;
  r: rect;

procedure show_point(px, py: integer);
begin
  writeln('(', px, ',', py, ')')
end;

function area(var rr: rect): integer;
begin
  area := (rr.right - rr.left) * (rr.bottom - rr.top)
end;

begin
  p.x := 3; p.y := 4;
  show_point(p.x, p.y);

  q.x := 10; q.y := 20;
  show_point(q.x, q.y);

  with q do
  begin
    x := x + 1;
    y := y + 1
  end;
  show_point(q.x, q.y);

  r.left := 0; r.top := 0; r.right := 5; r.bottom := 4;
  writeln('area=', area(r));

  with r do
  begin
    right := 10;
    bottom := 6
  end;
  writeln('area=', area(r));

  { local record }
end.
