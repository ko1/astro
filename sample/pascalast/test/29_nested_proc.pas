program NestedProcs;

procedure outer;
var
  x: integer;

  procedure inner;
  begin
    x := x + 1            { reaches outer's x }
  end;

  procedure inner_set(v: integer);
  begin
    x := v
  end;

begin
  x := 0;
  inner; inner; inner;
  writeln('after 3 inners: x=', x);
  inner_set(100);
  writeln('after set: x=', x)
end;

{ Mutual nested recursion within outer's scope. }
procedure spell(n: integer);
var
  base: integer;

  function rec(k: integer): integer;
  begin
    if k = 0 then rec := base
    else rec := rec(k - 1) + base
  end;

begin
  base := 10;
  writeln('spell(', n, ') = ', rec(n))
end;

{ Two levels of nesting. }
procedure deep;
var a: integer;

  procedure mid;
  var b: integer;

    procedure leaf;
    begin
      a := a + b   { reach grandparent's a, parent's b }
    end;

  begin
    b := 5;
    leaf;
    leaf
  end;

begin
  a := 0;
  mid;
  writeln('deep a=', a)
end;

begin
  outer;
  spell(4);
  deep
end.
