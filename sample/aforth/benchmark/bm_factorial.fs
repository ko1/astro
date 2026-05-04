\ Iterative factorial called repeatedly — loop + word call overhead.
\ fact(12) = 479001600 fits in int32 with margin; we sum 30M repeats which
\ stays within int64.

: fact ( n -- n! )
  1 SWAP 1+ 1 DO I * LOOP ;

: bench ( -- )
  0
  30000000 0 DO 12 fact + LOOP
  . CR ;

bench
