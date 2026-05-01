(* Booleans, comparisons, short-circuit *)
let pb b = if b then print_string "T" else print_string "F"
let _ = pb (1 < 2); pb (1 > 2); pb (1 = 1); pb (1 <> 2); print_newline ()
let _ = pb (1 <= 1); pb (1 >= 2); pb (true && false); pb (true || false); print_newline ()
let _ = pb (not true); pb (not false); pb (5 = 5 && 6 = 6); pb (5 = 6 || 7 = 7); print_newline ()
let _ = print_int (if true then 10 else 20); print_newline ()
let _ = print_int (if false then 10 else 20); print_newline ()
let _ = print_int (if 3 < 5 then 1 else 0); print_newline ()
