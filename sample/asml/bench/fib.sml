(* Fibonacci — classic recursive benchmark.
   fib(35) = 9227465.
   Portable between asml and SML/NJ; uses a local `println`. *)

fun println s = (print s; print "\n")

fun fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)

val _ = println (Int.toString (fib 35))
