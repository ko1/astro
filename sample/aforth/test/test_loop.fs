\ aforth — DO / LOOP / +LOOP / I / J / nested loops

\ sum 0..99
0 100 0 DO I + LOOP . CR     \ 4950

\ countdown via +LOOP step -1.  Note: `-1 SWAP DO ... -1 +LOOP` runs while
\ idx > -1 — so the count includes 0 (idx=0 then idx-1=-1 stops).
: count-down ( from to -- )
  -1 SWAP DO I . -1 +LOOP CR ;
0 5 count-down              \ 5 4 3 2 1 0

\ nested I/J
3 0 DO
  3 0 DO J . I . SPACE LOOP
LOOP CR

\ sum 1..1000
0 1001 1 DO I + LOOP . CR    \ 500500
