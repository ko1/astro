(* Mutual recursion with `and` *)

(* At top level (each side resolves via globals). *)
let rec is_even n =
  if n = 0 then true
  else is_odd (n - 1)
and is_odd n =
  if n = 0 then false
  else is_even (n - 1)

let _ = print_endline (if is_even 10 then "T" else "F")
let _ = print_endline (if is_odd 7 then "T" else "F")
let _ = print_endline (if is_even 7 then "T" else "F")
let _ = print_endline (if is_odd 100 then "T" else "F")

(* Nested let rec ... and ... in body *)
let _ =
  let rec ping n =
    if n = 0 then 0
    else 1 + pong (n - 1)
  and pong n =
    if n = 0 then 0
    else 1 + ping (n - 1)
  in
  print_int (ping 10); print_newline ();
  print_int (pong 5);  print_newline ()

(* `function` keyword form *)
let neg = function
  | true -> false
  | false -> true
let _ = print_endline (if neg true then "T" else "F")
let _ = print_endline (if neg false then "T" else "F")

let describe = function
  | 0 -> "zero"
  | 1 -> "one"
  | _ -> "other"
let _ = print_endline (describe 0)
let _ = print_endline (describe 1)
let _ = print_endline (describe 9)
