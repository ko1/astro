(* Tuples and destructuring *)
let pair = (1, 2)
let _ = match pair with (a, b) -> print_int a; print_string " "; print_int b; print_newline ()

let triple = (10, 20, 30)
let _ = match triple with (a, b, c) -> print_int (a + b + c); print_newline ()

let swap p = match p with (a, b) -> (b, a)
let _ = match swap (1, 2) with (a, b) -> print_int a; print_string " "; print_int b; print_newline ()

let fst p = match p with (a, _) -> a
let snd p = match p with (_, b) -> b
let _ = print_int (fst (100, 200)); print_newline ()
let _ = print_int (snd (100, 200)); print_newline ()

(* Tuple inside list *)
let pairs = [(1, "a"); (2, "b"); (3, "c")]
let rec print_pairs lst =
  match lst with
  | [] -> ()
  | (n, s) :: t ->
      print_int n; print_string ":"; print_string s; print_string " ";
      print_pairs t
let _ = print_pairs pairs; print_newline ()

(* Nested tuple *)
let nested = ((1, 2), (3, 4))
let _ = match nested with
  | ((a, b), (c, d)) -> print_int (a + b + c + d); print_newline ()
