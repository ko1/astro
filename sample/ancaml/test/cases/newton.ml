(* Newton's method for sqrt(2), to show float arithmetic + closures.  The
   result is scaled and truncated so output is integer-exact across
   implementations. *)
let rec newton x n =
  if n <= 0 then x
  else newton ((x +. 2.0 /. x) /. 2.0) (n - 1) in
let r = newton 1.0 10 in
print_int (int_of_float (r *. 1000000.0));
print_newline ()
