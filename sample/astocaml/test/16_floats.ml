(* Floats *)
let _ = print_float 3.14; print_newline ()
let _ = print_float (1.5 +. 2.25); print_newline ()
let _ = print_float (10.0 -. 3.5); print_newline ()
let _ = print_float (2.0 *. 3.5); print_newline ()
let _ = print_float (10.0 /. 4.0); print_newline ()

(* Float of int *)
let _ = print_float (float_of_int 42); print_newline ()
let _ = print_int (int_of_float 3.7); print_newline ()

(* Float comparisons (polymorphic) *)
let _ = if 1.5 < 2.5 then print_endline "lt-true" else print_endline "lt-false"
let _ = if 3.14 = 3.14 then print_endline "eq-true" else print_endline "eq-false"

(* Math primitives *)
let _ = print_float (sqrt 2.0); print_newline ()
let _ = print_float (floor 3.7); print_newline ()
let _ = print_float (ceil 3.2); print_newline ()

(* Recursive float computation *)
let rec power_f x n =
  if n = 0 then 1.0
  else x *. power_f x (n - 1)
let _ = print_float (power_f 2.0 10); print_newline ()

(* Compute pi via Leibniz series (small N for speed) *)
let rec leibniz n acc sign =
  if n = 0 then acc
  else leibniz (n - 1) (acc +. sign /. float_of_int (2 * n - 1)) (-. sign)
let _ = print_float (4.0 *. leibniz 1000 0.0 1.0); print_newline ()
