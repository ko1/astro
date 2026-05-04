\ GCD via subtraction — branch-heavy inner loop.

: gcd ( a b -- g )
  BEGIN
    2DUP <>
  WHILE
    2DUP > IF SWAP THEN     \ ensure a <= b   ( a b ) where a <= b
    OVER -                  \ ( a b-a )
  REPEAT
  DROP ;

: bench ( -- )
  0
  100000 0 DO
    I 100 + I 47 + gcd +
  LOOP
  . CR ;

bench
