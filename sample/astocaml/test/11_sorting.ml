(* Sorting algorithms *)

let rec print_list lst =
  match lst with
  | [] -> ()
  | h :: t ->
      print_int h; print_string " "; print_list t

(* Insertion sort *)
let rec insert x lst =
  match lst with
  | [] -> [x]
  | h :: t -> if x <= h then x :: lst else h :: insert x t

let rec isort lst =
  match lst with
  | [] -> []
  | h :: t -> insert h (isort t)

let _ = print_list (isort [5; 2; 8; 1; 9; 3; 7; 4; 6]); print_newline ()
let _ = print_list (isort []); print_newline ()
let _ = print_list (isort [42]); print_newline ()
let _ = print_list (isort [3; 1; 4; 1; 5; 9; 2; 6; 5; 3; 5; 8; 9; 7; 9]); print_newline ()

(* Quicksort: tuples aren't supported, so we split via two passes. *)
let rec lo_of pivot lst =
  match lst with
  | [] -> []
  | h :: t -> if h < pivot then h :: lo_of pivot t else lo_of pivot t

let rec hi_of pivot lst =
  match lst with
  | [] -> []
  | h :: t -> if h < pivot then hi_of pivot t else h :: hi_of pivot t

let rec append a b =
  match a with
  | [] -> b
  | h :: t -> h :: append t b

let rec qsort lst =
  match lst with
  | [] -> []
  | h :: t ->
      append (qsort (lo_of h t)) (h :: qsort (hi_of h t))

let _ = print_list (qsort [5; 2; 8; 1; 9; 3; 7; 4; 6]); print_newline ()
let _ = print_list (qsort [3; 1; 4; 1; 5; 9; 2; 6; 5; 3; 5]); print_newline ()
