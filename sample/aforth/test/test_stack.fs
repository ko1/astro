\ aforth — stack manipulation smoke test

1 2 OVER . . . CR        \ a b -- a b a; prints (top first) 1 2 1
1 2 3 ROT . . . CR       \ a b c -- b c a; prints 3 1 2
1 2 NIP . CR             \ a b -- b; prints 2
1 2 TUCK . . . CR        \ a b -- b a b; prints 2 1 2
1 2 2DUP . . . . CR      \ a b -- a b a b; prints 2 1 2 1
1 2 SWAP . . CR          \ swap two; prints 1 2
1 2 3 4 2DROP . . CR     \ drop 2; prints 2 1
