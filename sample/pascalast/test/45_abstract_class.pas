program AbstractClassMethod;
type
  TShape = class
    constructor Create;
    procedure draw; virtual; abstract;
    function name: string; virtual; abstract;
  end;

  TCircle = class(TShape)
    procedure draw; override;
    function  name: string; override;
  end;

  TUtil = class
    class function twice(x: integer): integer;
    class function greet(s: string): string;
  end;

constructor TShape.Create;
begin end;

procedure TCircle.draw;
begin writeln('drawing circle') end;

function TCircle.name: string;
begin name := 'circle' end;

class function TUtil.twice(x: integer): integer;
begin twice := x * 2 end;

class function TUtil.greet(s: string): string;
begin greet := 'hello, ' + s + '!' end;

var
  s: TShape;
  c: TCircle;
begin
  c := TCircle.Create;
  c.draw;
  writeln('name: ', c.name);

  { polymorphic via base type }
  s := c;
  s.draw;
  writeln('name: ', s.name);

  { class method — no instance needed }
  writeln('twice 21 = ', TUtil.twice(21));
  writeln(TUtil.greet('world'));

  { calling abstract on base directly raises }
  s := TShape.Create;
  try
    s.draw
  except
    writeln('caught: ', ExceptionMessage)
  end
end.
