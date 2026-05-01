program Exceptions;

procedure dangerous(n: integer);
begin
  if n = 0 then raise 'zero not allowed';
  if n < 0 then raise 'negative not allowed';
  writeln('ok with ', n)
end;

procedure with_finally;
begin
  writeln('enter');
  try
    raise 'mid-fail'
  finally
    writeln('cleanup ran')
  end
end;

var i: integer;

begin
  { try/except — straight catch }
  try
    dangerous(5);
    dangerous(0);
    dangerous(99)             { not reached }
  except
    writeln('caught: ', ExceptionMessage)
  end;

  { try/except inside a loop — re-enterable }
  for i := -1 to 1 do
  begin
    try
      dangerous(i)
    except
      writeln('loop ', i, ' caught: ', ExceptionMessage)
    end
  end;

  { try/finally — finally always runs, exception re-raised when uncaught }
  try
    with_finally
  except
    writeln('outer caught: ', ExceptionMessage)
  end
end.
