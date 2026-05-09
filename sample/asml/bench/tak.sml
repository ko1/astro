(* Takeuchi function.  tak(24,16,8) = 9, looped 5 times for sustained
   measurement. *)

fun tak (x, y, z) =
  if x <= y then z
  else tak (tak (x - 1, y, z), tak (y - 1, z, x), tak (z - 1, x, y))

fun rep n acc =
  if n = 0 then acc else rep (n - 1) (tak (24, 16, 8))

val _ = println (Int.toString (rep 5 0))
