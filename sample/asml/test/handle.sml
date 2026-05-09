fun safeDiv (a, b) =
  (a div b) handle Div => 0

val _ = println (Int.toString (safeDiv (10, 3)))
val _ = println (Int.toString (safeDiv (10, 0)))
