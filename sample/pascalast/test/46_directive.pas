program DirectiveTest;
{ exercise compiler directives — $R+ enables, $R- disables range checks }
var
  age: 0..150;
  i: integer;
begin
  {$R+}
  age := 42;
  writeln('with R+: age=', age);

  try
    age := 200
  except
    writeln('caught: ', ExceptionMessage)
  end;

  {$R-}
  { now range checks are off — assigning 200 should succeed silently }
  age := 200;
  writeln('with R-: age=', age);

  {$R+}
  { back on — should raise again }
  try
    age := 999
  except
    writeln('caught2: ', ExceptionMessage)
  end;

  { unknown directives accepted silently }
  {$H+}
  {$MODE OBJFPC}
  i := 7;
  writeln('done i=', i)
end.
