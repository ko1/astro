(* Random / Sys / Format / succ / pred / bool conversions *)

let _ = Random.init 42
let _ = print_int (Random.int 100); print_newline ()
let _ = print_int (Random.int 100); print_newline ()

let _ = print_int (succ 7); print_newline ()
let _ = print_int (pred 7); print_newline ()

let _ = print_endline (string_of_bool true)
let _ = print_endline (string_of_bool false)

let _ = print_endline (if bool_of_string "true" then "T" else "F")
let _ = print_endline (if bool_of_string "false" then "T" else "F")

(* Format / Printf compat *)
let _ = Format.printf "x = %d, name = %s\n" 42 "alice"
let _ = print_endline (Format.sprintf "%d + %d = %d" 1 2 3)
