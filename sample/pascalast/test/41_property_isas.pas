program PropertyIsAs;
type
  TPoint = class
    private
      FX, FY: integer;
    public
      constructor Create(x, y: integer);
      function GetMagSq: integer;
      property X: integer read FX write FX;
      property Y: integer read FY write FY;
      property MagSq: integer read GetMagSq;     { read-only }
  end;

  TColored = class(TPoint)
    private
      FColor: integer;
    public
      constructor Create(x, y, c: integer);
      property Color: integer read FColor write FColor;
  end;

constructor TPoint.Create(x, y: integer);
begin
  FX := x; FY := y
end;

function TPoint.GetMagSq: integer;
begin
  GetMagSq := FX * FX + FY * FY
end;

constructor TColored.Create(x, y, c: integer);
begin
  inherited Create(x, y);
  FColor := c
end;

procedure describe(p: TPoint);
begin
  if p is TColored then
    writeln('colored point (', p.X, ',', p.Y, ') color=', (p as TColored).Color, ' mag2=', p.MagSq)
  else
    writeln('plain point (', p.X, ',', p.Y, ') mag2=', p.MagSq)
end;

var
  p: TPoint;
  q: TColored;
begin
  p := TPoint.Create(3, 4);
  q := TColored.Create(5, 12, 7);

  describe(p);
  describe(q);

  { property write }
  p.X := 100; p.Y := 200;
  writeln('moved p: (', p.X, ',', p.Y, ')');

  q.Color := 9;
  writeln('q color: ', q.Color)
end.
