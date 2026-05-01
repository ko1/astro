program ForwardDecl;
{ Mutual recursion via forward; — even/odd, Hofstadter F/M. }

function odd_p(n: integer): boolean; forward;

function even_p(n: integer): boolean;
begin
  if n = 0 then even_p := true
  else even_p := odd_p(n - 1)
end;

function odd_p(n: integer): boolean;
begin
  if n = 0 then odd_p := false
  else odd_p := even_p(n - 1)
end;

function hM(n: integer): integer; forward;

function hF(n: integer): integer;
begin
  if n = 0 then hF := 1
  else hF := n - hM(hF(n - 1))
end;

function hM(n: integer): integer;
begin
  if n = 0 then hM := 0
  else hM := n - hF(hM(n - 1))
end;

var i: integer;
begin
  for i := 0 to 5 do
  begin
    write(i, ': ');
    if even_p(i) then write('even ') else write('odd ');
    writeln('hF=', hF(i), ' hM=', hM(i))
  end
end.
