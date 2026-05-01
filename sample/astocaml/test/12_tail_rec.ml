(* Tail recursion (we don't TCO but small cases work) *)
let rec sum_to acc n =
  if n = 0 then acc
  else sum_to (acc + n) (n - 1)
let _ = print_int (sum_to 0 1000); print_newline ()
let _ = print_int (sum_to 0 10000); print_newline ()

let rec count_down n =
  if n = 0 then ()
  else count_down (n - 1)
let _ = count_down 10000; print_endline "done counting down"

(* Accumulator-style reverse *)
let rec rev_aux acc lst =
  match lst with
  | [] -> acc
  | h :: t -> rev_aux (h :: acc) t

let rec print_list lst =
  match lst with
  | [] -> ()
  | h :: t -> print_int h; print_string " "; print_list t

let _ = print_list (rev_aux [] [1; 2; 3; 4; 5]); print_newline ()

(* Range generator *)
let rec range_aux acc n =
  if n = 0 then acc
  else range_aux (n :: acc) (n - 1)
let range n = range_aux [] n

let rec length lst =
  match lst with
  | [] -> 0
  | _ :: t -> 1 + length t

let _ = print_int (length (range 100)); print_newline ()
let _ = print_list (range 10); print_newline ()
