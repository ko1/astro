(* sumlist — リストを構築して総和を計算。cons / pat-test / lref が
   ホットパスに乗る。 直接的な末尾再帰版 (curry 経由の partial application を
   避ける)。 *)

fun println s = (print s; print "\n")

fun range (b, acc) =
  if b < 1 then acc else range (b - 1, b :: acc)

fun sum (xs, acc) =
  case xs of
      []     => acc
    | h :: t => sum (t, h + acc)

val xs = range (1000000, [])
val _  = println (Int.toString (sum (xs, 0)))
