(* References and mutation *)
let r = ref 0
let _ = r := 42
let _ = print_int !r; print_newline ()

let counter = ref 0
let bump () = counter := !counter + 1
let _ = bump (); bump (); bump (); bump ()
let _ = print_int !counter; print_newline ()

(* Ref inside closure *)
let make_counter () =
  let c = ref 0 in
  fun () -> c := !c + 1; !c

let c1 = make_counter ()
let _ = print_int (c1 ()); print_newline ()
let _ = print_int (c1 ()); print_newline ()
let _ = print_int (c1 ()); print_newline ()

let c2 = make_counter ()
let _ = print_int (c2 ()); print_newline ()
let _ = print_int (c1 ()); print_newline ()

(* Imperative loop over a list *)
let sum_list lst =
  let total = ref 0 in
  let rec loop l =
    match l with
    | [] -> ()
    | h :: t -> total := !total + h; loop t
  in
  loop lst;
  !total
let _ = print_int (sum_list [1; 2; 3; 4; 5]); print_newline ()
