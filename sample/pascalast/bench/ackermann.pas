program AckBench;
{ Ackermann — deep recursion.  ack(3, 10) = 8189. }
function ack(m, n: integer): integer;
begin
  if m = 0 then ack := n + 1
  else if n = 0 then ack := ack(m - 1, 1)
  else ack := ack(m - 1, ack(m, n - 1))
end;
begin
  writeln(ack(3, 10))
end.
