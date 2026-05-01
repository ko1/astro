(* List literals, cons, basic operations *)
let _ = print_int (length []); print_newline ()
let _ = print_int (length [1; 2; 3; 4; 5]); print_newline ()
let _ = print_int (length (1 :: 2 :: 3 :: [])); print_newline ()

let rec sum lst =
  match lst with
  | [] -> 0
  | h :: t -> h + sum t

let rec list_max lst =
  match lst with
  | [] -> 0
  | h :: t ->
      let m = list_max t in
      if h > m then h else m

let _ = print_int (sum [1; 2; 3; 4; 5; 6; 7; 8; 9; 10]); print_newline ()
let _ = print_int (list_max [3; 1; 4; 1; 5; 9; 2; 6; 5; 3; 5]); print_newline ()

let rec rev_aux acc lst =
  match lst with
  | [] -> acc
  | h :: t -> rev_aux (h :: acc) t
let rev lst = rev_aux [] lst

let rec print_int_list lst =
  match lst with
  | [] -> ()
  | h :: t ->
      print_int h;
      print_string " ";
      print_int_list t

let _ = print_int_list (rev [1; 2; 3; 4; 5]); print_newline ()

(* append *)
let rec append a b =
  match a with
  | [] -> b
  | h :: t -> h :: append t b
let _ = print_int_list (append [1; 2; 3] [4; 5; 6]); print_newline ()
let _ = print_int_list (append [] [10; 20]); print_newline ()
let _ = print_int_list (append [1] []); print_newline ()
