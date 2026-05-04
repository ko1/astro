\ Takeuchi function — recursion + comparison + arithmetic.
\ tak(x, y, z) = if y < x then tak( tak(x-1,y,z), tak(y-1,z,x), tak(z-1,x,y) )
\                         else z
\ tak(18, 12, 6) = 7.
\
\ aforth has no native locals, so we use VARIABLE Tx/Ty/Tz as a scratch
\ frame and save/restore the previous values on the return stack across
\ recursion so each invocation sees its own copy.

VARIABLE Tx  VARIABLE Ty  VARIABLE Tz

: tak ( x y z -- v )
  Tx @ Ty @ Tz @                 \ save old: ( ... oTx oTy oTz )
  >R >R >R                       \ to rstack (R-top: oTx)
  Tz ! Ty ! Tx !                 \ store new args
  Ty @ Tx @ < IF
    Tx @ 1- Ty @ Tz @ RECURSE
    Ty @ 1- Tz @ Tx @ RECURSE
    Tz @ 1- Tx @ Ty @ RECURSE
    RECURSE
  ELSE
    Tz @
  THEN
  R> Tx !  R> Ty !  R> Tz ! ;     \ restore in reverse push order

24 16 8 tak . CR

