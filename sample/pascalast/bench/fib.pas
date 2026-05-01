program FibBench;
{ classic exponential fib — exercises function call + tail-recursive
  branchy code paths.  fib(36)=14930352. }
function fib(n: integer): integer;
begin
  if n < 2 then fib := n
  else fib := fib(n - 1) + fib(n - 2)
end;
begin
  writeln(fib(36))
end.
