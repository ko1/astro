program BoolTest;
var
  a, b: boolean;
  i: integer;

begin
  a := true;
  b := false;
  if a and not b then writeln('case1');
  if a or b then writeln('case2');
  if not (a and b) then writeln('case3');

  { short circuit: divide by zero would explode if not short-circuited }
  i := 0;
  if (i = 0) or (10 div i = 0) then writeln('shortor');
  if (i <> 0) and (10 div i = 0) then writeln('bad') else writeln('shortand');

  for i := 1 to 5 do
    if odd(i) then writeln(i, ' is odd')
    else writeln(i, ' is even')
end.
