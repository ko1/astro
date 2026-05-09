val ans =
  let
    val x = 10
    val y = 20
    fun add (a, b) = a + b
  in
    add (x, y)
  end

val _ = println (Int.toString ans)

(* Mutual recursion *)
val isodd =
  let
    fun even n = if n = 0 then true else odd (n - 1)
    and odd n  = if n = 0 then false else even (n - 1)
  in
    odd
  end

val _ = if isodd 7 then println "yes" else println "no"
val _ = if isodd 8 then println "yes" else println "no"
