(* Higher-order functions: map, fold, filter *)
let rec map f lst =
  match lst with
  | [] -> []
  | h :: t -> f h :: map f t

let rec fold_left f acc lst =
  match lst with
  | [] -> acc
  | h :: t -> fold_left f (f acc h) t

let rec fold_right f lst acc =
  match lst with
  | [] -> acc
  | h :: t -> f h (fold_right f t acc)

let rec filter p lst =
  match lst with
  | [] -> []
  | h :: t -> if p h then h :: filter p t else filter p t

let rec print_list lst =
  match lst with
  | [] -> ()
  | h :: t ->
      print_int h;
      print_string " ";
      print_list t

let _ =
  let doubled = map (fun x -> x * 2) [1; 2; 3; 4; 5] in
  print_list doubled; print_newline ()

let _ =
  let s = fold_left (fun a b -> a + b) 0 [1; 2; 3; 4; 5; 6; 7; 8; 9; 10] in
  print_int s; print_newline ()

let _ =
  let p = fold_left (fun a b -> a * b) 1 [1; 2; 3; 4; 5] in
  print_int p; print_newline ()

let _ =
  let lst = fold_right (fun x acc -> x :: acc) [1; 2; 3] [10; 20; 30] in
  print_list lst; print_newline ()

let _ =
  let evens = filter (fun n -> n mod 2 = 0) [1; 2; 3; 4; 5; 6; 7; 8; 9; 10] in
  print_list evens; print_newline ()

(* compose *)
let compose f g x = f (g x)
let _ = print_int (compose (fun x -> x + 1) (fun x -> x * 2) 5); print_newline ()
