(* Fibonacci — classic recursive benchmark.
 * fib(35) = 9227465. The "Computer Language Benchmarks Game" canonical
 * recursive form. *)

let rec fib n =
  if n < 2 then n
  else fib (n - 1) + fib (n - 2)

let _ = print_int (fib 35); print_newline ()
