(* Polymorphism via let-poly + value restriction *)
fun id x = x

val a = id 5
val b = id "hi"
val c = id (1, 2)

val _ = println (Int.toString a)
val _ = println b
val _ = println (Int.toString (case c of (x, _) => x))

(* Polymorphic list functions *)
fun length xs = case xs of [] => 0 | _ :: t => 1 + length t
fun rev xs =
  let
    fun loop xs acc = case xs of [] => acc | h :: t => loop t (h :: acc)
  in loop xs [] end

val _ = println (Int.toString (length [1, 2, 3, 4]))
val _ = println (Int.toString (length ["a", "b"]))
val rl = rev [1, 2, 3]
val _ = println (Int.toString (case rl of h :: _ => h | _ => 0))
