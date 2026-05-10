(* strcat — 文字列を 100,000 回繰り返し連結。
   ml_string_concat (heap allocation + memcpy) のホット計測。 *)

fun println s = (print s; print "\n")

fun build (i, n, acc) =
  if i >= n then acc
  else build (i + 1, n, acc ^ "x")

val s = build (0, 30000, "")
val _ = println (Int.toString (String.size s))
