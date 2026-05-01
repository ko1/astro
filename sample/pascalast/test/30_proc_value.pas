program ProcValues;
type
  TIntProc = procedure(x: integer);
  TIntFn   = function(x: integer): integer;

var
  p: TIntProc;
  f: TIntFn;
  i: integer;

procedure printer(x: integer);
begin writeln('val=', x) end;

procedure shouter(x: integer);
begin writeln('!! ', x) end;

function dbl(x: integer): integer;
begin dbl := x * 2 end;

function sq(x: integer): integer;
begin sq := x * x end;

procedure apply(g: TIntFn; x: integer);
begin
  writeln('apply -> ', g(x))
end;

begin
  p := @printer;
  p(10);
  p := @shouter;
  p(20);

  f := @dbl;
  writeln(f(5));
  f := @sq;
  writeln(f(7));

  for i := 1 to 4 do
  begin
    if (i mod 2) = 0 then p := @printer else p := @shouter;
    p(i)
  end;

  apply(@dbl, 11);
  apply(@sq,  9)
end.
