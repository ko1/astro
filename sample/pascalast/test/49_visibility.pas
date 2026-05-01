program VisibilityTest;
{ visibility enforcement: private/protected fields can be accessed
  inside the declaring class's methods (and protected from descendants),
  but not from outside.  Public is always allowed. }

type
  TBase = class
  private
    secret: integer;
  protected
    family: integer;
  public
    open: integer;
    constructor Create(s, f, o: integer);
    function ShowSecret: integer;     { allowed: same class }
    function ShowFamily: integer;     { allowed: same class }
  end;

  TChild = class(TBase)
  public
    function GetFamily: integer;      { allowed: protected, descendant }
    function TryGetSecret: integer;   { caller side test — see below }
  end;

constructor TBase.Create(s, f, o: integer);
begin
  secret := s;
  family := f;
  open   := o
end;

function TBase.ShowSecret: integer;
begin
  ShowSecret := secret               { OK — private from same class }
end;

function TBase.ShowFamily: integer;
begin
  ShowFamily := family               { OK — protected from same class }
end;

function TChild.GetFamily: integer;
begin
  GetFamily := family                { OK — protected, descendant }
end;

function TChild.TryGetSecret: integer;
begin
  { secret is private to TBase — child cannot reach it.  We don't
    actually try (would be a compile error).  Just return 0 to keep
    the program well-formed. }
  TryGetSecret := 0
end;

var b: TBase;
    c: TChild;
begin
  b := TBase.Create(11, 22, 33);
  writeln('b.open=', b.open);          { OK — public }
  writeln('b.ShowSecret=', b.ShowSecret);
  writeln('b.ShowFamily=', b.ShowFamily);

  c := TChild.Create(7, 8, 9);
  writeln('c.GetFamily=', c.GetFamily);
  writeln('c.TryGetSecret=', c.TryGetSecret)
end.
