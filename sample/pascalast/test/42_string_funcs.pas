program StringFuncs;
var
  s, t: string;
  i: integer;
  r: real;
begin
  s := 'hello, world';
  writeln('length=', length(s));
  writeln('copy 8..12: "', copy(s, 8, 5), '"');
  writeln('pos "world": ', pos('world', s));
  writeln('pos "x": ', pos('x', s));

  t := 'abcdef';
  insert('XYZ', t, 4);
  writeln('insert: ', t);

  delete(t, 4, 3);
  writeln('delete: ', t);

  setlength(t, 3);
  writeln('truncated: "', t, '"');

  setlength(t, 8);
  writeln('extended: "', t, '" (len ', length(t), ')');

  s := IntToStr(42) + '/' + IntToStr(-7);
  writeln(s);

  i := StrToInt('100') + StrToInt('23');
  writeln(i);

  r := StrToFloat('3.14');
  writeln(r:0:4);
  writeln(FloatToStr(r * 2.0))
end.
