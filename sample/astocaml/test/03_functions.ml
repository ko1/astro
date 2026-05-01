(* Function definitions and applications *)
let id x = x
let inc x = x + 1
let add x y = x + y
let sum3 a b c = a + b + c
let sum4 a b c d = a + b + c + d
let sum5 a b c d e = a + b + c + d + e
let sum6 a b c d e f = a + b + c + d + e + f

let _ = print_int (id 42); print_newline ()
let _ = print_int (inc 41); print_newline ()
let _ = print_int (add 3 4); print_newline ()
let _ = print_int (sum3 1 2 3); print_newline ()
let _ = print_int (sum4 1 2 3 4); print_newline ()
let _ = print_int (sum5 1 2 3 4 5); print_newline ()
let _ = print_int (sum6 1 2 3 4 5 6); print_newline ()

(* anonymous functions *)
let _ = print_int ((fun x -> x * 2) 7); print_newline ()
let _ = print_int ((fun x y -> x + y) 10 20); print_newline ()

(* nested function calls *)
let _ = print_int (inc (inc (inc 0))); print_newline ()

(* let f x = ... in body *)
let _ =
  let double x = x * 2 in
  print_int (double 21); print_newline ()
