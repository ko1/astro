\ Nested DO loop — pure dispatch + counter arith, no allocations.
\ Computes sum_{i=0..N-1} sum_{j=0..N-1} (i*j).

: matsum ( n -- s )
  0 SWAP                        \ ( accum n )
  DUP 0 DO
    DUP 0 DO
      I J * ROT + SWAP          \ accum += i*j
    LOOP
  LOOP
  DROP ;

8000 matsum . CR
