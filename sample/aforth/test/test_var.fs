\ aforth — VARIABLE / CONSTANT / CREATE+ALLOT smoke test

VARIABLE counter
0 counter !

: bump ( -- ) 1 counter +! ;
: peek ( -- )  counter @ . CR ;

bump bump bump
peek                \ 3

42 CONSTANT magic
magic . CR          \ 42

\ small array via CREATE / ALLOT.  Each cell is sizeof(VALUE) bytes; we
\ index by adding `i CELLS` to the base address.
CREATE arr  10 ALLOT

: aset ( v i -- )  CELLS arr + ! ;
: aget ( i -- v )  CELLS arr + @ ;

10 0 aset
20 1 aset
30 2 aset

0 aget . 1 aget . 2 aget . CR    \ 10 20 30
