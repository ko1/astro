\ aforth — arithmetic smoke test
\ expected output: 7 -1 12 4 1 -5 5

1 2 + 4 + .       \ 7
3 4 - .           \ -1
3 4 * .           \ 12
13 3 / .          \ 4
13 3 MOD .        \ 1
5 NEGATE .        \ -5
-5 ABS .          \ 5
CR
