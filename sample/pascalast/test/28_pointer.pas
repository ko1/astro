program PointerTest;
type
  TNode = record value: integer; next: integer end;
  pNode = ^TNode;

var
  p, q: pNode;
  i: integer;

begin
  new(p);
  p^.value := 10;
  p^.next  := 0;

  new(q);
  q^.value := 20;
  q^.next  := 0;

  writeln(p^.value, ' ', q^.value);

  { build a tiny linked list using ints-as-indices is too clunky.
    Instead, just test pointer = nil. }
  if p = nil then writeln('p is nil') else writeln('p is set');

  dispose(q);
  dispose(p);

  p := nil;
  if p = nil then writeln('after nil')
end.
