(* recordsum — record の literal allocation + field selection を多数。
   500,000 個の {x, y} を作って、それぞれの x+y を total に積む。 *)

fun println s = (print s; print "\n")

fun loop (i, n, total) =
  if i >= n then total
  else
    let val p = {x = i, y = i + 1}
    in loop (i + 1, n, total + #x p + #y p) end

val _ = println (Int.toString (loop (0, 500000, 0)))
