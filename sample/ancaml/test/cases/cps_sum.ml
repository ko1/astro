(* Continuation-passing sum 1..n.  Each step builds a fresh continuation
   closure (k2) capturing `k` and `n`, so `sum_cps` is NOT a leaf — this
   exercises the heap-frame closure path and tail calls into continuations.
   MinCaml has no anonymous `fun`, so the continuation is a named `let rec`. *)
let rec id x = x in
let rec sum_cps n k =
  if n = 0 then k 0
  else sum_cps (n - 1) (let rec k2 r = k (r + n) in k2) in
print_int (sum_cps 100 id);
print_newline ()
