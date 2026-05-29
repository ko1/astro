(* Takeuchi function — a classic recursion micro-benchmark. *)
let rec tak x y z =
  if x <= y then z
  else tak (tak (x - 1) y z) (tak (y - 1) z x) (tak (z - 1) x y) in
print_int (tak 12 6 0);
print_newline ()
