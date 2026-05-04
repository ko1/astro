\ Ackermann(3, n) — deep recursion.  ack(3, n) = 2^(n+3) - 3.
\ ack(3, 8) = 2045; recursion depth ~2k, well within native C stack.

: ack ( m n -- v )
  OVER 0= IF
    NIP 1+
  ELSE
    DUP 0= IF
      DROP 1- 1 RECURSE
    ELSE
      OVER >R                 \ save m on rstack
      1- RECURSE              \ ack(m, n-1) -> a; ( a ), R: m
      R> 1- SWAP              \ ( m-1 a )
      RECURSE                 \ ack(m-1, a)
    THEN
  THEN ;

\ run a few times for sustained scale (~1s on interp).
20 0 DO 3 8 ack DROP LOOP
3 8 ack . CR
