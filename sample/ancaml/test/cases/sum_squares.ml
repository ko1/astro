(* Sum of i squared for i in 1..n, using float multiplication, since
   MinCaml has no integer product; then truncated back to int. *)
let rec sumsq i =
  if i <= 0 then 0.0
  else float_of_int i *. float_of_int i +. sumsq (i - 1) in
print_int (int_of_float (sumsq 30));
print_newline ()
