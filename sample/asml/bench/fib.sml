(* Fibonacci — classic recursive benchmark.
   fib(35) = 9227465. *)

fun fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)

val _ = println (Int.toString (fib 35))
