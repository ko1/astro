(* String literals, concatenation, output *)
let _ = print_string "hello, world"; print_newline ()
let _ = print_endline "single line"
let _ = print_string ("foo" ^ "bar"); print_newline ()
let _ = print_endline ("a" ^ "b" ^ "c")

(* string_of_int *)
let _ = print_endline (string_of_int 42)
let _ = print_endline (string_of_int (- 5))
let _ = print_endline ("answer = " ^ string_of_int 42)

(* int_of_string *)
let _ = print_int (int_of_string "123"); print_newline ()
let _ = print_int (int_of_string "0"); print_newline ()

(* Built-in print_endline does newline automatically *)
let _ = print_endline "line1"
let _ = print_endline "line2"

(* String equality *)
let _ = if "abc" = "abc" then print_endline "eq" else print_endline "neq"
let _ = if "abc" = "abd" then print_endline "eq" else print_endline "neq"

(* Escape sequences *)
let _ = print_endline "tab:\there"
let _ = print_string "no_nl"; print_string "_after"; print_newline ()
