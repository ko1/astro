\ aforth — IF / BEGIN / DO control-flow smoke test

\ IF/ELSE/THEN
: pos? ( n -- )
  DUP 0> IF
    ." positive: " . CR
  ELSE
    ." nonpositive: " . CR
  THEN ;

3 pos?
0 pos?
-7 pos?

\ BEGIN ... UNTIL
: countdown ( n -- )
  BEGIN
    DUP .         \ print
    1-
    DUP 0=
  UNTIL
  DROP CR ;

5 countdown

\ DO ... LOOP and I
: triangle ( n -- )
  0 DO
    I 0 DO ." *" LOOP CR
  LOOP ;

5 triangle
