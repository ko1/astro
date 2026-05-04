\ Sieve of Eratosthenes — array store / fetch + nested loops.
\ Counts primes below N.

500000 CONSTANT N
CREATE flags  N CELLS ALLOT       \ N cells, default 0 means "prime"

: clear-flags ( -- )
  N 0 DO 0 I CELLS flags + ! LOOP ;

: sieve ( -- count )
  0                                 \ ( count )
  N 2 DO                            \ outer i = I
    I CELLS flags + @ 0= IF         \ flags[i] == 0 → prime
      1+
      I I * N < IF
        N I I * DO                  \ inner k = I; goes from i*i .. N-1 step i
          1 I CELLS flags + !       \   flags[k] = 1
        J +LOOP                     \   step = J (outer i)
      THEN
    THEN
  LOOP ;

\ Sustained scale: ~1s on interp.  primes below 500000 = 41538.
\ Wrapped for gforth (DO/LOOP is compile-only at toplevel in standard Forth).
: main
  20 0 DO clear-flags sieve DROP LOOP
  clear-flags sieve . CR ;          \ 41538
main
