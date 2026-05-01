program ClassTest;
type
  TAnimal = class
    name: string;
    legs: integer;
    constructor Create(n: string; l: integer);
    procedure speak;
    function describe: string;
  end;

  TDog = class(TAnimal)
    procedure speak;
  end;

constructor TAnimal.Create(n: string; l: integer);
begin
  Self.name := n;
  Self.legs := l
end;

procedure TAnimal.speak;
begin
  writeln(Self.name, ' makes a generic sound')
end;

function TAnimal.describe: string;
begin
  describe := Self.name + ' (legs: x)'
end;

procedure TDog.speak;
begin
  writeln(Self.name, ' barks!')
end;

var
  a: TAnimal;
  d: TDog;
begin
  a := TAnimal.Create('cat', 4);
  a.speak;
  writeln(a.describe);
  writeln('legs=', a.legs);

  d := TDog.Create('rex', 4);
  d.speak;
  writeln(d.describe)
end.
