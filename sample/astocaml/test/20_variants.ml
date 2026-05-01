(* User-defined variants and option type *)
type 'a option = None | Some of 'a

let print_opt_int o =
  match o with
  | None -> print_endline "none"
  | Some n -> print_int n; print_newline ()

let _ = print_opt_int None
let _ = print_opt_int (Some 42)
let _ = print_opt_int (Some 0)

(* Tree variant *)
type tree = Leaf | Node of int * tree * tree

let rec sum_tree t =
  match t with
  | Leaf -> 0
  | Node (v, l, r) -> v + sum_tree l + sum_tree r

let t1 = Node (1, Node (2, Leaf, Leaf), Node (3, Leaf, Node (4, Leaf, Leaf)))
let _ = print_int (sum_tree t1); print_newline ()

let rec depth t =
  match t with
  | Leaf -> 0
  | Node (_, l, r) ->
      let dl = depth l in
      let dr = depth r in
      1 + (if dl > dr then dl else dr)
let _ = print_int (depth t1); print_newline ()

(* Either-like variant *)
type 'a either = Left of int | Right of 'a

let switch e =
  match e with
  | Left n -> Right (n * 2)
  | Right v -> Left v

let _ =
  match switch (Left 5) with
  | Left n -> print_string "L:"; print_int n; print_newline ()
  | Right n -> print_string "R:"; print_int n; print_newline ()

let _ =
  match switch (Right 100) with
  | Left n -> print_string "L:"; print_int n; print_newline ()
  | Right n -> print_string "R:"; print_int n; print_newline ()
