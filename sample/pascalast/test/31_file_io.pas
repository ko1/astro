program FileIO;
var
  f: text;
  i, n, sum: integer;
  line: string;

begin
  { write some data }
  assign(f, '/tmp/pascalast_test.txt');
  rewrite(f);
  for i := 1 to 5 do writeln(f, i, ' ', i * i);
  close(f);

  { read it back }
  assign(f, '/tmp/pascalast_test.txt');
  reset(f);
  sum := 0;
  while not eof(f) do
  begin
    read(f, n);
    read(f, n);          { just take the squared value too }
    sum := sum + n;
    readln(f)            { eat newline }
  end;
  close(f);
  writeln('sum of i + i*i pairs = ', sum);

  { read text lines }
  assign(f, '/tmp/pascalast_test.txt');
  reset(f);
  while not eof(f) do
  begin
    readln(f, line);
    writeln('> ', line)
  end;
  close(f)
end.
