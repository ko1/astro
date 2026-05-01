(* Type errors should be detected.  We capture them via try/with on the
 * runtime Type_error exception (the static checker also prints to
 * stderr, but stdout is what we compare). *)

let try_str f =
  try f (); print_endline "ok"
  with Type_error _ -> print_endline "type-error"

(* int + string *)
let _ = try_str (fun () -> ignore ("a" + 1))

(* bool + int *)
let _ = try_str (fun () -> ignore (true + 5))

(* if condition not bool *)
let _ = try_str (fun () -> if 5 then ignore 1 else ignore 0)

(* float +. int *)
let _ = try_str (fun () -> ignore (1.5 +. 2))

(* string ^ int *)
let _ = try_str (fun () -> ignore ("foo" ^ 1))

(* not on int *)
let _ = try_str (fun () -> ignore (not 5))

(* if branches differ *)
let _ = try_str (fun () -> ignore (if true then 1 else "two"))
