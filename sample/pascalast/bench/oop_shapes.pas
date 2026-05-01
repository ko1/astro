program OopShapes;
(* virtual-dispatch micro-benchmark.

   Three subclasses of TShape override area + perimeter.  We allocate
   one of each and loop M times, summing area + perimeter on each.
   Each iteration does six virtual dispatches.  M sized so the loop
   runs ~1 s on this machine.

   This bench focuses on virtual-dispatch *cost*, not allocation —
   the static array element-type restriction (integer/boolean/real/
   string only) means we can't easily build a polymorphic array
   without resorting to integer-cast pointer tricks.  Fixed-shape
   loop is enough to exercise the vtable path. *)

type
  TShape = class
  public
    side: integer;
    function area: integer; virtual;
    function perimeter: integer; virtual;
    constructor Create(s: integer);
  end;

  TRect = class(TShape)
  public
    function area: integer; override;
    function perimeter: integer; override;
  end;

  TCircle = class(TShape)
  public
    function area: integer; override;
    function perimeter: integer; override;
  end;

  TTriangle = class(TShape)
  public
    function area: integer; override;
    function perimeter: integer; override;
  end;

constructor TShape.Create(s: integer); begin side := s end;
function TShape.area: integer;      begin area := 0 end;
function TShape.perimeter: integer; begin perimeter := 0 end;

function TRect.area: integer;       begin area := side * (side + 1) end;
function TRect.perimeter: integer;  begin perimeter := 2 * (side + side + 1) end;
function TCircle.area: integer;     begin area := 3 * side * side end;
function TCircle.perimeter: integer;begin perimeter := 6 * side end;
function TTriangle.area: integer;     begin area := side * side div 2 end;
function TTriangle.perimeter: integer;begin perimeter := 3 * side end;

var
  r: TRect;
  c: TCircle;
  t: TTriangle;
  base: TShape;
  i, total: integer;
begin
  r := TRect.Create(7);
  c := TCircle.Create(5);
  t := TTriangle.Create(11);

  total := 0;
  for i := 1 to 7000000 do
  begin
    base := r; total := total + base.area + base.perimeter;
    base := c; total := total + base.area + base.perimeter;
    base := t; total := total + base.area + base.perimeter
  end;

  writeln(total)
end.
