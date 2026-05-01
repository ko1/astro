(* Closures: capture environment *)
let make_adder n = fun x -> x + n
let add5 = make_adder 5
let add10 = make_adder 10
let _ = print_int (add5 3); print_newline ()
let _ = print_int (add10 3); print_newline ()
let _ = print_int (make_adder 100 50); print_newline ()

(* Counter via closure (no mutation; just demonstrates capture) *)
let counter_at start = fun delta -> start + delta
let c1 = counter_at 1000
let _ = print_int (c1 5); print_newline ()
let _ = print_int (c1 50); print_newline ()

(* Returning a function that uses inner let *)
let mk_mult m =
  let times x = m * x in
  times

let triple = mk_mult 3
let _ = print_int (triple 7); print_newline ()
let _ = print_int (mk_mult 10 11); print_newline ()

(* Closure capturing multiple variables *)
let combine a b c =
  fun x -> a * x + b * x + c

let f = combine 1 2 3
let _ = print_int (f 10); print_newline ()
let _ = print_int (f 0); print_newline ()
