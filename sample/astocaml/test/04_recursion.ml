(* Recursive functions *)
let rec fact n = if n = 0 then 1 else n * fact (n - 1)
let rec fib n = if n < 2 then n else fib (n - 1) + fib (n - 2)
let rec gcd a b = if b = 0 then a else gcd b (a mod b)
let rec power b n = if n = 0 then 1 else b * power b (n - 1)
let rec sum_to n = if n = 0 then 0 else n + sum_to (n - 1)

let _ = print_int (fact 0); print_newline ()
let _ = print_int (fact 5); print_newline ()
let _ = print_int (fact 10); print_newline ()
let _ = print_int (fib 0); print_newline ()
let _ = print_int (fib 10); print_newline ()
let _ = print_int (fib 20); print_newline ()
let _ = print_int (gcd 12 18); print_newline ()
let _ = print_int (gcd 100 75); print_newline ()
let _ = print_int (power 2 10); print_newline ()
let _ = print_int (power 3 4); print_newline ()
let _ = print_int (sum_to 100); print_newline ()

(* Ackermann (small inputs only) *)
let rec ack m n =
  if m = 0 then n + 1
  else if n = 0 then ack (m - 1) 1
  else ack (m - 1) (ack m (n - 1))

let _ = print_int (ack 2 3); print_newline ()
let _ = print_int (ack 3 3); print_newline ()
