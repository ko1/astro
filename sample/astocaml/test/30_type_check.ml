(* Type system regression tests.
 *
 * Things that *should* type-check (and run): no errors expected.
 * Things that *shouldn't* type-check are exercised in 30_type_errors,
 * which uses --no-check + try/with to run despite (and document) the
 * runtime type errors. *)

(* Plain int / float arithmetic — fine *)
let _ = print_int (1 + 2 * 3 - 4); print_newline ()
let _ = print_float (1.5 +. 2.5 *. 3.0); print_newline ()

(* String concat — fine *)
let _ = print_endline ("hello, " ^ "world")

(* Comparison polymorphism — operands of same type are fine *)
let _ = print_endline (if 1 < 2 then "T" else "F")
let _ = print_endline (if "abc" = "abc" then "T" else "F")
let _ = print_endline (if [1;2;3] = [1;2;3] then "T" else "F")

(* Function: if/let/recursion infer cleanly *)
let rec fact n = if n = 0 then 1 else n * fact (n - 1)
let _ = print_int (fact 5); print_newline ()

(* List ops *)
let rec sum lst =
  match lst with
  | [] -> 0
  | h :: t -> h + sum t
let _ = print_int (sum [1; 2; 3; 4; 5]); print_newline ()

(* Refs *)
let r = ref 0
let _ = r := !r + 100
let _ = print_int !r; print_newline ()

(* If-then-else with same-type branches *)
let abs_int n = if n < 0 then - n else n
let _ = print_int (abs_int (- 7)); print_newline ()
