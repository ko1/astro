(* let bindings, shadowing, let-in *)
let x = 10
let y = 20
let _ = print_int (x + y); print_newline ()

let _ =
  let a = 5 in
  let b = 7 in
  print_int (a + b); print_newline ()

(* shadowing *)
let _ =
  let x = 100 in
  print_int x; print_newline ();
  let x = x + 1 in
  print_int x; print_newline ()

(* outer x still 10 *)
let _ = print_int x; print_newline ()

(* nested let *)
let _ =
  let z =
    let inner = 3 in
    inner * 4
  in
  print_int z; print_newline ()

(* multiple sequenced let-in *)
let _ =
  let p = 1 in
  let q = p + 1 in
  let r = q + 1 in
  let s = r + 1 in
  print_int (p + q + r + s); print_newline ()
