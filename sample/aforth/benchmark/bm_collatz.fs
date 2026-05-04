\ Sum of Collatz step counts for 1..N — branch-heavy loop, integer arith.

: collatz-len ( n -- steps )
  0 SWAP
  BEGIN
    DUP 1 >
  WHILE
    SWAP 1+ SWAP                  \ inc step counter
    DUP 2 MOD 0= IF
      2 /
    ELSE
      3 * 1+
    THEN
  REPEAT
  DROP ;

: total ( N -- sum )
  0 SWAP 1+ 1 DO
    I collatz-len +
  LOOP ;

200000 total . CR     \ sum of collatz lengths for 1..200000
