(* Extended List.* operations *)

let rec print_list lst =
  match lst with
  | [] -> ()
  | h :: t -> print_int h; print_string " "; print_list t

let _ = print_list (List.sort compare [3; 1; 4; 1; 5; 9; 2; 6; 5]); print_newline ()
let _ = print_list (List.init 5 (fun i -> i * i)); print_newline ()
let _ = print_list (List.flatten [[1;2]; [3;4]; [5]]); print_newline ()

let _ = print_endline (if List.exists (fun x -> x > 5) [1;2;3;6;7] then "T" else "F")
let _ = print_endline (if List.for_all (fun x -> x > 0) [1;2;3] then "T" else "F")
let _ = print_endline (if List.for_all (fun x -> x > 0) [1;-1;3] then "T" else "F")

let _ = print_int (List.find (fun x -> x > 3) [1;2;3;4;5;6]); print_newline ()

let _ =
  match List.find_opt (fun x -> x > 100) [1;2;3] with
  | None -> print_endline "none"
  | Some n -> print_int n; print_newline ()

(* assoc list *)
let env = [("alpha", 1); ("beta", 2); ("gamma", 3)]
let _ = print_int (List.assoc "beta" env); print_newline ()
let _ = print_endline (if List.mem_assoc "delta" env then "T" else "F")

(* combine / split *)
let xs = [1; 2; 3]
let ys = [10; 20; 30]
let pairs = List.combine xs ys
let _ =
  let rec p l = match l with
    | [] -> ()
    | (a, b) :: t -> print_int a; print_string ","; print_int b; print_string " "; p t
  in
  p pairs; print_newline ()

let (la, lb) = List.split pairs
let _ = print_list la; print_newline ()
let _ = print_list lb; print_newline ()

(* partition *)
let _ =
  let (evens, odds) = List.partition (fun n -> n mod 2 = 0) [1;2;3;4;5;6;7;8;9;10] in
  print_list evens; print_newline ();
  print_list odds; print_newline ()

(* iter2 / map2 *)
let _ = List.iter2 (fun a b -> print_int (a + b); print_string " ") [1;2;3] [10;20;30]; print_newline ()
let _ = print_list (List.map2 (fun a b -> a * b) [1;2;3] [10;20;30]); print_newline ()
