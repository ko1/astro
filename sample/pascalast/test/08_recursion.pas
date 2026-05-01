program Recursion;

function gcd(a, b: integer): integer;
begin
  if b = 0 then gcd := a
  else gcd := gcd(b, a mod b)
end;

function pow(base, exp: integer): integer;
begin
  if exp = 0 then pow := 1
  else if odd(exp) then pow := base * pow(base, exp - 1)
  else pow := sqr(pow(base, exp div 2))
end;

function tarai(x, y, z: integer): integer;
begin
  if x <= y then tarai := y
  else tarai := tarai(tarai(x - 1, y, z),
                      tarai(y - 1, z, x),
                      tarai(z - 1, x, y))
end;

begin
  writeln(gcd(252, 105));
  writeln(pow(2, 10));
  writeln(pow(3, 13));
  writeln(tarai(8, 4, 0))
end.
