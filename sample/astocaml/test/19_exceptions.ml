(* try / with / raise / failwith *)

let _ =
  try failwith "oops"
  with Failure msg -> print_endline ("caught: " ^ msg)

let _ =
  try
    let _ = 1 / 0 in
    print_endline "unreachable"
  with Division_by_zero -> print_endline "div0"

let _ =
  try
    raise Not_found
  with Not_found -> print_endline "nf"

(* Re-raising / nested handlers *)
let _ =
  try
    try failwith "inner"
    with Not_found -> print_endline "nf-inner"
  with Failure m -> print_endline ("outer caught: " ^ m)

(* Exception with multiple constructors *)
exception My_exn of int

let _ =
  try raise (My_exn 42)
  with My_exn n -> print_int n; print_newline ()

(* assert *)
let _ =
  try assert true; print_endline "ok-true"
  with _ -> print_endline "no"
let _ =
  try assert false; print_endline "no"
  with Assert_failure -> print_endline "ok-false"

(* invalid_arg *)
let _ =
  try invalid_arg "bad"
  with Invalid_argument m -> print_endline ("inv: " ^ m)
