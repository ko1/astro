unit myunit;
interface

const
  GREETING = 'hello from myunit';

procedure greet;
function  square(x: integer): integer;

implementation

procedure greet;
begin
  writeln(GREETING)
end;

function square(x: integer): integer;
begin
  square := x * x
end;

end.
