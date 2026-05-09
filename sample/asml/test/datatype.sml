datatype 'a option = NONE | SOME of 'a

fun unwrap x = case x of NONE => 0 | SOME v => v

val _ = println (Int.toString (unwrap (SOME 42)))
val _ = println (Int.toString (unwrap NONE))

datatype shape = Circle of int | Square of int | Triangle

fun area s =
  case s of
      Circle r   => 3 * r * r
    | Square s   => s * s
    | Triangle   => 0

val _ = println (Int.toString (area (Circle 5)))
val _ = println (Int.toString (area (Square 4)))
val _ = println (Int.toString (area Triangle))
