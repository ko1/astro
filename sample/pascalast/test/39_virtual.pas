program VirtualDispatch;
type
  TAnimal = class
    name: string;
    constructor Create(n: string);
    procedure speak; virtual;
    function describe: string; virtual;
  end;

  TDog = class(TAnimal)
    procedure speak; override;
  end;

  TCat = class(TAnimal)
    procedure speak; override;
    function describe: string; override;
  end;

constructor TAnimal.Create(n: string);
begin
  Self.name := n
end;

procedure TAnimal.speak;
begin writeln(Self.name, ' makes a sound') end;

function TAnimal.describe: string;
begin describe := 'an animal called ' + Self.name end;

procedure TDog.speak;
begin writeln(Self.name, ' barks!') end;

procedure TCat.speak;
begin writeln(Self.name, ' meows...') end;

function TCat.describe: string;
begin describe := 'a cat named ' + Self.name end;

procedure show(a: TAnimal);
begin
  a.speak;                  { virtual: dispatches to actual class's speak }
  writeln('-> ', a.describe)
end;

var
  generic: TAnimal;
  d: TDog;
  c: TCat;
begin
  d := TDog.Create('rex');
  c := TCat.Create('mia');
  generic := TAnimal.Create('thing');

  { Direct dispatch — uses the static type, but for virtual it still
    goes through the vtable so child overrides win. }
  d.speak;
  c.speak;
  generic.speak;

  writeln('--');

  { Polymorphic dispatch: assign to a base-typed variable, virtual
    methods still pick the override. }
  show(d);
  show(c);
  show(generic);

  writeln('--');

  { Mixed array of polymorphic instances. }
  show(TDog.Create('buddy'));
  show(TCat.Create('whiskers'))
end.
