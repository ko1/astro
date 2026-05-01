(* Modules (basic) *)
module Math = struct
  let pi = 3.14159
  let square x = x * x
  let cube x = x * x * x
end

let _ = print_float Math.pi; print_newline ()
let _ = print_int (Math.square 7); print_newline ()
let _ = print_int (Math.cube 3); print_newline ()

(* Nested module *)
module Outer = struct
  module Inner = struct
    let v = 42
    let double x = x + x
  end
  let outer_fn x = x + Inner.v
end

let _ = print_int Outer.Inner.v; print_newline ()
let _ = print_int (Outer.Inner.double 21); print_newline ()
let _ = print_int (Outer.outer_fn 8); print_newline ()

(* `open` brings names into top-level scope *)
open Math
let _ = print_int (square 9); print_newline ()
let _ = print_int (cube 4); print_newline ()

(* Module with mutual rec *)
module Parity = struct
  let rec is_even n =
    if n = 0 then true else is_odd (n - 1)
  and is_odd n =
    if n = 0 then false else is_even (n - 1)
end

let _ = print_endline (if Parity.is_even 10 then "T" else "F")
let _ = print_endline (if Parity.is_odd 7 then "T" else "F")

(* lazy / Lazy.force *)
let lz = lazy (print_string "[evaluated] "; 42)
let _ = print_int (Lazy.force lz); print_newline ()
let _ = print_int (Lazy.force lz); print_newline ()    (* cached, no print *)

(* Printf.printf *)
let _ = Printf.printf "x = %d, name = %s, pi = %.3f\n" 7 "alice" 3.14159
let _ = Printf.printf "%d + %d = %d\n" 1 2 (1 + 2)

(* Bytes *)
let b = Bytes.create 5
let _ = Bytes.set b 0 65
let _ = Bytes.set b 1 66
let _ = Bytes.set b 2 67
let _ = Bytes.set b 3 68
let _ = Bytes.set b 4 69
let _ = print_endline (Bytes.to_string b)
let _ = print_int (Bytes.length b); print_newline ()
let _ = print_int (Bytes.get b 2); print_newline ()
