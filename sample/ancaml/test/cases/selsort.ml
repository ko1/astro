(* Selection sort on an int array.  The array is filled in descending order
   (n, n-1, ..., 1); after sorting it must be 1..n, which the order-sensitive
   checksum verifies.  Exercises arrays, nested tail recursion, and swaps. *)
let n = 40 in
let a = Array.create n 0 in
let rec fill i = if i >= n then () else (a.(i) <- n - i; fill (i + 1)) in
fill 0;
let rec swap i j = (let t = a.(i) in a.(i) <- a.(j); a.(j) <- t) in
let rec minpos i j m = if j >= n then m else minpos i (j + 1) (if a.(j) < a.(m) then j else m) in
let rec sel i = if i >= n then () else (swap i (minpos i (i + 1) i); sel (i + 1)) in
sel 0;
let rec check i acc = if i >= n then acc else check (i + 1) (acc + (i + 1) + a.(i)) in
print_int (check 0 0);
print_newline ()
