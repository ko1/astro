\ Tight inner-loop array sum — pure @ + + + DO/LOOP.

10000 CONSTANT N
CREATE arr  N ALLOT

\ initialize arr[i] = i
: init  N 0 DO I I CELLS arr + ! LOOP ;

: sum ( -- s )
  0
  N 0 DO
    I CELLS arr + @ +
  LOOP ;

init
0
8000 0 DO sum + LOOP        \ repeat for sustained scale (~1s interp)
. CR
