(* Takeuchi function — classic deeply-recursive benchmark.
 * tak(18,12,6) = 7, with ~63 million calls. *)

let rec tak x y z =
  if x <= y then z
  else tak (tak (x - 1) y z) (tak (y - 1) z x) (tak (z - 1) x y)

(* Loop a few times to get a sustained ~1 s reading. *)
let rec rep n acc =
  if n = 0 then acc
  else rep (n - 1) (tak 24 16 8)

let _ = print_int (rep 5 0); print_newline ()
