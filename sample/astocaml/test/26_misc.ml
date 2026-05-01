(* Miscellaneous: or-pattern, match with exception, infix def,
 * polymorphic variant, == / !=, .5 float syntax. *)

(* Or-pattern *)
let class_int n =
  match n with
  | 1 | 2 | 3 -> "small"
  | 10 | 20 | 30 -> "medium"
  | _ -> "other"
let _ = print_endline (class_int 2)
let _ = print_endline (class_int 30)
let _ = print_endline (class_int 100)

(* match | exception E -> handler *)
let safe_apply f x =
  match f x with
  | exception (Failure m) -> "fail: " ^ m
  | exception Not_found -> "not found"
  | v -> "ok: " ^ string_of_int v

let _ = print_endline (safe_apply (fun n -> n * 2) 21)
let _ = print_endline (safe_apply (fun _ -> failwith "boom") 0)
let _ = print_endline (safe_apply (fun _ -> raise Not_found) 0)

(* Infix operator definition (single-char op only — multi-char custom
 * operators like `+!` aren't lex-recognized by our minimal lexer). *)
let plus3 a b c = a + b + c
let _ = print_int (plus3 1 2 3); print_newline ()
let _ = print_int ((+) 5 9); print_newline ()
let _ = print_endline (if (<) 1 2 then "T" else "F")

(* Polymorphic variant *)
let poly_classify v =
  match v with
  | `Foo -> "foo"
  | `Bar n -> "bar(" ^ string_of_int n ^ ")"
  | `Baz (a, b) -> "baz(" ^ string_of_int a ^ "," ^ string_of_int b ^ ")"

let _ = print_endline (poly_classify `Foo)
let _ = print_endline (poly_classify (`Bar 42))
let _ = print_endline (poly_classify (`Baz (1, 2)))

(* == / != physical *)
let s1 = "abc"
let s2 = "abc"
let _ = print_endline (if s1 = s2 then "structural-eq" else "no")
let _ = print_endline (if s1 == s2 then "phys-eq" else "phys-neq")    (* same literal: probably distinct objs *)

(* `.5` float syntax *)
let _ = print_float .5; print_newline ()
let _ = print_float (.5 +. .25); print_newline ()
let _ = print_float .125; print_newline ()
