\ Tight inner-loop array sum — pure @ + + + DO/LOOP.

10000 CONSTANT N
CREATE arr  N CELLS ALLOT

\ initialize arr[i] = i
: init  N 0 DO I I CELLS arr + ! LOOP ;

: sum ( -- s )
  0
  N 0 DO
    I CELLS arr + @ +
  LOOP ;

\ Wrapped for gforth (DO/LOOP is compile-only at toplevel in standard Forth).
: main
  init
  0  8000 0 DO sum + LOOP   \ repeat for sustained scale (~1s interp)
  . CR ;
main
