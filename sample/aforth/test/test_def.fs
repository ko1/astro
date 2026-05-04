\ aforth — word definition + RECURSE smoke test

: square ( n -- n^2 ) DUP * ;
: cube   ( n -- n^3 ) DUP square * ;

3 square . CR     \ 9
4 cube . CR       \ 64

\ recursive fib (Forth-canonical, no EXIT — uses IF/ELSE)
: fib ( n -- f )
  DUP 2 < IF
    \ 0 -> 0, 1 -> 1
  ELSE
    DUP 1- RECURSE
    SWAP 2 - RECURSE
    +
  THEN ;

10 fib . CR       \ 55
20 fib . CR       \ 6765

\ ackermann
: ack ( m n -- a )
  OVER 0= IF
    NIP 1+
  ELSE
    DUP 0= IF
      DROP 1- 1 RECURSE
    ELSE
      OVER 1- >R
      OVER SWAP 1- RECURSE
      R> SWAP RECURSE
    THEN
  THEN ;

2 3 ack . CR      \ 9
3 3 ack . CR      \ 61
