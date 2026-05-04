: fib ( n -- f )
  DUP 2 < IF
  ELSE
    DUP 1- RECURSE
    SWAP 2 - RECURSE
    +
  THEN ;

30 fib . CR
