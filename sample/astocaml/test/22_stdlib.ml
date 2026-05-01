(* Stdlib basics: List.* / String.* / Array.* *)

(* List.map *)
let _ =
  let r = List.map (fun x -> x * x) [1; 2; 3; 4; 5] in
  let rec p l = match l with [] -> () | h :: t -> print_int h; print_string " "; p t in
  p r; print_newline ()

(* List.filter *)
let _ =
  let r = List.filter (fun x -> x mod 2 = 0) [1; 2; 3; 4; 5; 6] in
  let rec p l = match l with [] -> () | h :: t -> print_int h; print_string " "; p t in
  p r; print_newline ()

(* List.fold_left *)
let _ = print_int (List.fold_left (fun a b -> a + b) 0 [1; 2; 3; 4; 5; 6; 7; 8; 9; 10]); print_newline ()
let _ = print_int (List.fold_left (fun a b -> a * b) 1 [1; 2; 3; 4; 5]); print_newline ()

(* List.length, hd, tl, rev *)
let _ = print_int (List.length [10; 20; 30]); print_newline ()
let _ = print_int (List.hd [42; 1; 2]); print_newline ()
let _ = print_int (List.length (List.tl [42; 1; 2; 3])); print_newline ()

let _ =
  let r = List.rev [1; 2; 3; 4; 5] in
  let rec p l = match l with [] -> () | h :: t -> print_int h; print_string " "; p t in
  p r; print_newline ()

(* List.append *)
let _ =
  let r = List.append [1; 2; 3] [4; 5; 6] in
  print_int (List.length r); print_newline ()

(* List.iter *)
let _ = List.iter (fun n -> print_int n; print_string ":") [10; 20; 30]; print_newline ()

(* List.mem *)
let _ = print_endline (if List.mem 3 [1; 2; 3; 4; 5] then "yes" else "no")
let _ = print_endline (if List.mem 99 [1; 2; 3; 4; 5] then "yes" else "no")

(* List.nth *)
let _ = print_int (List.nth [10; 20; 30; 40] 2); print_newline ()

(* String.length / String.sub *)
let _ = print_int (String.length "hello"); print_newline ()
let _ = print_endline (String.sub "hello world" 6 5)

(* Array.make / Array.length / Array.get / Array.set *)
let _ =
  let a = Array.make 5 0 in
  Array.set a 0 10;
  Array.set a 1 20;
  Array.set a 2 30;
  print_int (Array.length a); print_newline ();
  print_int (Array.get a 0); print_string " ";
  print_int (Array.get a 1); print_string " ";
  print_int (Array.get a 2); print_newline ()

(* Array literal + index *)
let _ =
  let a = [| 100; 200; 300 |] in
  print_int a.(0); print_string " ";
  print_int a.(2); print_newline ()

(* compare *)
let _ = print_int (compare 1 2); print_newline ()
let _ = print_int (compare 5 5); print_newline ()
let _ = print_int (compare "abc" "abd"); print_newline ()

(* min/max/abs *)
let _ = print_int (min 3 7); print_newline ()
let _ = print_int (max 3 7); print_newline ()
let _ = print_int (abs (- 5)); print_newline ()
