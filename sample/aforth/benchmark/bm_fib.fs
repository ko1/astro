\ Recursive Fibonacci — call-heavy + IF/RECURSE.  fib(35) ~0.6s interp.

: fib ( n -- f )
  DUP 2 < IF
  ELSE
    DUP 1- RECURSE
    SWAP 2 - RECURSE
    +
  THEN ;

36 fib . CR
