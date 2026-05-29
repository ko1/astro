(* Fill an array 0..n-1, reverse it in place, and print a checksum that
   weights each slot by its index (so order matters). *)
let n = 10 in
let a = Array.create n 0 in
let rec fill i = if i >= n then () else (a.(i) <- i; fill (i + 1)) in
fill 0;
let rec rev i j =
  if i >= j then ()
  else (let t = a.(i) in a.(i) <- a.(j); a.(j) <- t; rev (i + 1) (j - 1)) in
rev 0 (n - 1);
let rec checksum i = if i >= n then 0 else (i + 1) + a.(i) + checksum (i + 1) in
print_int (checksum 0);
print_newline ()
