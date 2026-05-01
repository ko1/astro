(* Records *)
type point = { x : int; y : int }

let p = { x = 3; y = 4 }
let _ = print_int p.x; print_string " "; print_int p.y; print_newline ()

let move dx dy p = { x = p.x + dx; y = p.y + dy }
let p2 = move 10 20 p
let _ = print_int p2.x; print_string " "; print_int p2.y; print_newline ()

(* Functional update *)
let p3 = { p with x = 100 }
let _ = print_int p3.x; print_string " "; print_int p3.y; print_newline ()
let _ = print_int p.x; print_string " "; print_int p.y; print_newline ()    (* original unchanged *)

(* Pattern matching on records *)
let dist_origin p =
  match p with
  | { x = a; y = b } -> a * a + b * b

let _ = print_int (dist_origin { x = 3; y = 4 }); print_newline ()

(* Punning: { x } means { x = x } *)
let mk_point x y = { x; y }
let _ =
  let q = mk_point 7 8 in
  print_int q.x; print_string " "; print_int q.y; print_newline ()

(* Records inside lists *)
let pts = [{ x = 1; y = 1 }; { x = 2; y = 4 }; { x = 3; y = 9 }]

let rec sum_xs lst =
  match lst with
  | [] -> 0
  | h :: t -> h.x + sum_xs t

let _ = print_int (sum_xs pts); print_newline ()
