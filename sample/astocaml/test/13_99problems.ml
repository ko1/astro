(* Adapted from the classic "99 OCaml Problems" set
 * (https://ocaml.org/exercises). Original solutions use OCaml features we
 * don't support (the `function` keyword, nested patterns inside ::,
 * polymorphic variants); this port rewrites them in our subset.  *)

(* P01: Last element of a list. *)
let rec last lst =
  match lst with
  | [] -> 0  (* sentinel for empty (real OCaml uses 'a option) *)
  | x :: t -> if t = [] then x else last t

let _ = print_int (last [1; 2; 3; 4; 5]); print_newline ()
let _ = print_int (last [42]); print_newline ()
let _ = print_int (last []); print_newline ()

(* P03: Find the K'th element (0-indexed). *)
let rec at k lst =
  match lst with
  | [] -> 0
  | h :: t -> if k = 0 then h else at (k - 1) t

let _ = print_int (at 2 [1; 2; 3; 4; 5]); print_newline ()
let _ = print_int (at 0 [10; 20; 30]); print_newline ()

(* P04: Number of elements. *)
let rec mylen lst =
  match lst with
  | [] -> 0
  | h :: t -> 1 + mylen t
let _ = print_int (mylen [1; 2; 3; 4; 5; 6; 7]); print_newline ()

(* P05: Reverse a list (tail recursive). *)
let rec myrev_aux acc lst =
  match lst with
  | [] -> acc
  | h :: t -> myrev_aux (h :: acc) t
let myrev lst = myrev_aux [] lst

let rec print_list lst =
  match lst with
  | [] -> ()
  | h :: t -> print_int h; print_string " "; print_list t

let _ = print_list (myrev [1; 2; 3; 4; 5]); print_newline ()

(* P09: Pack consecutive duplicates into sublists.
 * Simplified: count runs of consecutive equals and print run-length pairs. *)
let rec encode_aux x count lst =
  match lst with
  | [] -> [count; x]                (* flatten: [count; value] *)
  | h :: t ->
      if h = x then encode_aux x (count + 1) t
      else count :: x :: encode_aux h 1 t

let encode lst =
  match lst with
  | [] -> []
  | h :: t -> encode_aux h 1 t

(* Output is a flat list of [count, value, count, value, ...] *)
let _ = print_list (encode [1; 1; 1; 2; 3; 3; 4; 4; 4; 4]); print_newline ()

(* P14: Duplicate the elements of a list. *)
let rec duplicate lst =
  match lst with
  | [] -> []
  | h :: t -> h :: h :: duplicate t
let _ = print_list (duplicate [1; 2; 3]); print_newline ()

(* P17: Split a list into two parts: first n, then the rest.
 * Tuples not supported -> we print the two halves on separate lines. *)
let rec take n lst =
  if n = 0 then []
  else match lst with
       | [] -> []
       | h :: t -> h :: take (n - 1) t
let rec drop n lst =
  if n = 0 then lst
  else match lst with
       | [] -> []
       | h :: t -> drop (n - 1) t
let _ = print_list (take 3 [1; 2; 3; 4; 5; 6]); print_newline ()
let _ = print_list (drop 3 [1; 2; 3; 4; 5; 6]); print_newline ()

(* P22: List of integers in a given range, lo..hi. *)
let rec range lo hi =
  if lo > hi then []
  else lo :: range (lo + 1) hi
let _ = print_list (range 4 9); print_newline ()
let _ = print_list (range 1 1); print_newline ()
let _ = print_list (range 5 4); print_newline ()
