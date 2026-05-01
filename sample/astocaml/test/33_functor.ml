(* Functor instantiation *)

module type ORD = sig
  type t
  val cmp : t -> t -> int
end

module IntOrd = struct
  type t = int
  let cmp a b = compare a b
end

module StrOrd = struct
  type t = string
  let cmp a b = compare a b
end

(* Two equivalent syntaxes for functor declaration: *)
module Make1 (X : ORD) = struct
  let leq a b = X.cmp a b <= 0
  let max a b = if X.cmp a b >= 0 then a else b
end

module Make2 = functor (X : ORD) -> struct
  let geq a b = X.cmp a b >= 0
  let min a b = if X.cmp a b <= 0 then a else b
end

module IntM = Make1 (IntOrd)
module StrM = Make1 (StrOrd)
module IntN = Make2 (IntOrd)

let _ = print_endline (if IntM.leq 3 5 then "T" else "F")
let _ = print_endline (if IntM.leq 8 5 then "T" else "F")
let _ = print_int (IntM.max 7 12); print_newline ()

let _ = print_endline (if StrM.leq "abc" "abd" then "T" else "F")
let _ = print_endline (StrM.max "alpha" "zebra")

let _ = print_endline (if IntN.geq 5 5 then "T" else "F")
let _ = print_int (IntN.min 7 3); print_newline ()
