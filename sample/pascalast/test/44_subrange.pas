program SubrangeTest;
var
  age: 0..150;
  digit: 0..9;
  i: integer;
begin
  age := 42;
  writeln('age=', age);

  digit := 0;
  for i := 0 to 9 do
  begin
    digit := i;
    write(digit, ' ')
  end;
  writeln;

  { trying to assign out-of-range value should raise }
  try
    age := 200
  except
    writeln('caught: ', ExceptionMessage)
  end
end.
