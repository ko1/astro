(* let-polymorphism: a single `id` used at multiple types *)
let id x = x
let _ = print_int (id 42); print_newline ()
let _ = print_endline (id "hello")
let _ = print_endline (if id true then "T" else "F")

(* Polymorphic pair *)
let pair x y = (x, y)
let _ =
  let (a, b) = pair 1 "abc" in
  print_int a; print_string " "; print_endline b

(* Polymorphic compose *)
let compose f g x = f (g x)
let inc x = x + 1
let twice = compose inc inc
let _ = print_int (twice 5); print_newline ()

(* Polymorphic identity through globals *)
let apply f x = f x
let _ = print_int (apply inc 100); print_newline ()
let _ = print_endline (apply (fun s -> s ^ "!") "hi")

(* fst / snd *)
let fst p = match p with (a, _) -> a
let snd p = match p with (_, b) -> b
let _ = print_int (fst (10, "ten")); print_newline ()
let _ = print_endline (snd (10, "ten"))

(* Higher-order *)
let rec list_map f lst =
  match lst with
  | [] -> []
  | h :: t -> f h :: list_map f t
let rec print_list f lst =
  match lst with
  | [] -> ()
  | h :: t -> f h; print_string " "; print_list f t
let _ = print_list print_int (list_map (fun n -> n * 2) [1; 2; 3]); print_newline ()
let _ = print_list print_string (list_map (fun s -> s ^ "!") ["a"; "b"; "c"]); print_newline ()
