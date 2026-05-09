fun map f xs =
  case xs of
      []      => []
    | h :: t  => f h :: map f t

fun foldl f acc xs =
  case xs of
      []     => acc
    | h :: t => foldl f (f (h, acc)) t

fun sum xs = foldl (fn (a, b) => a + b) 0 xs

val xs   = [1, 2, 3, 4, 5]
val ys   = map (fn x => x * x) xs
val tot  = sum ys
val _    = println (Int.toString tot)
val _    = println (Int.toString (List.length ys))
