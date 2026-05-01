(* Tail-call optimization.  Without TCO, deep recursion blows the C
 * stack; with TCO, these run with constant stack via the trampoline in
 * oc_apply. *)

let rec count_down n =
  if n = 0 then ()
  else count_down (n - 1)
let _ = count_down 1000000; print_endline "1M down ok"

let rec sum_to acc n =
  if n = 0 then acc
  else sum_to (acc + n) (n - 1)
let _ = print_int (sum_to 0 100000); print_newline ()
let _ = print_int (sum_to 0 1000000); print_newline ()

(* Mutual tail recursion (top-level). *)
let rec even_p n = if n = 0 then true else odd_p (n - 1)
and odd_p n = if n = 0 then false else even_p (n - 1)

let _ = print_endline (if even_p 100000 then "T" else "F")
let _ = print_endline (if odd_p  99999  then "T" else "F")

(* Tail call inside match arm. *)
let rec drop n lst =
  if n = 0 then lst
  else
    match lst with
    | [] -> []
    | _ :: t -> drop (n - 1) t

let rec range_aux acc n =
  if n = 0 then acc
  else range_aux (n :: acc) (n - 1)
let range n = range_aux [] n

let _ =
  let lst = range 50000 in
  match drop 49999 lst with
  | [] -> print_endline "(empty)"
  | h :: _ -> print_int h; print_newline ()
