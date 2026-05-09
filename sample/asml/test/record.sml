(* Records. *)

fun println s = (print s; print "\n")

(* Literal + #field selector *)
val p = {x = 10, y = 20}
val _ = println (Int.toString (#x p + #y p))

(* Record pattern with full names *)
fun len {x, y} = x + y
val _ = println (Int.toString (len p))

(* Pattern with explicit `f = pat` *)
fun first {x = a, y = _} = a
val _ = println (Int.toString (first p))

(* Records as values returned from a function *)
fun mkpt (x, y) = {x = x, y = y}
val q = mkpt (3, 4)
val _ = println (Int.toString (#x q + #y q))

(* Records inside datatypes *)
datatype shape = Pt of {x : int, y : int}

val s = Pt {x = 5, y = 6}
val n = case s of Pt {x, y} => x * 100 + y
val _ = println (Int.toString n)

(* Records compared structurally *)
val r1 = {a = 1, b = 2}
val r2 = {b = 2, a = 1}    (* same record value, different field order *)
val _ = if r1 = r2 then println "eq" else println "ne"
