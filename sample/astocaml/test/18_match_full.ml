(* Nested patterns and match guards *)

(* Nested cons pattern *)
let rec firstn n lst =
  match lst with
  | _ when n = 0 -> []
  | [] -> []
  | h :: t -> h :: firstn (n - 1) t

let rec print_list lst =
  match lst with
  | [] -> ()
  | h :: t -> print_int h; print_string " "; print_list t
let _ = print_list (firstn 3 [10; 20; 30; 40; 50]); print_newline ()
let _ = print_list (firstn 0 [10; 20; 30]); print_newline ()
let _ = print_list (firstn 100 [1; 2; 3]); print_newline ()

(* Tuple in pattern *)
let safer_div p =
  match p with
  | (_, 0) -> 0
  | (a, b) -> a / b
let _ = print_int (safer_div (10, 0));  print_newline ()
let _ = print_int (safer_div (10, 3));  print_newline ()
let _ = print_int (safer_div (100, 4)); print_newline ()

(* Match with guard *)
let classify n =
  match n with
  | x when x < 0 -> "negative"
  | 0 -> "zero"
  | x when x < 10 -> "small"
  | x when x < 100 -> "medium"
  | _ -> "large"
let _ = print_endline (classify (- 5))
let _ = print_endline (classify 0)
let _ = print_endline (classify 5)
let _ = print_endline (classify 50)
let _ = print_endline (classify 500)

(* As-pattern *)
let head_or_zero lst =
  match lst with
  | [] -> 0
  | (h :: _) as _whole -> h
let _ = print_int (head_or_zero []); print_newline ()
let _ = print_int (head_or_zero [42; 1; 2]); print_newline ()
