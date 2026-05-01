(* Pattern matching variants *)
let _ =
  let n = 3 in
  match n with
  | 0 -> print_string "zero"
  | 1 -> print_string "one"
  | 2 -> print_string "two"
  | 3 -> print_string "three"
  | _ -> print_string "many"
let _ = print_newline ()

let describe n =
  match n with
  | 0 -> "nil"
  | 1 -> "single"
  | _ -> "many"

let _ = print_endline (describe 0)
let _ = print_endline (describe 1)
let _ = print_endline (describe 5)

(* Match on bool *)
let yn b =
  match b with
  | true -> "Y"
  | false -> "N"
let _ = print_endline (yn true)
let _ = print_endline (yn false)

(* Match on list *)
let head_or n lst =
  match lst with
  | [] -> n
  | h :: _ -> h
let _ = print_int (head_or 99 []); print_newline ()
let _ = print_int (head_or 99 [42; 1; 2]); print_newline ()

(* Match with variable binding *)
let safe_div a b =
  match b with
  | 0 -> 0
  | n -> a / n
let _ = print_int (safe_div 100 0); print_newline ()
let _ = print_int (safe_div 100 4); print_newline ()
