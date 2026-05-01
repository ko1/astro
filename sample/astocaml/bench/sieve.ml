(* Sieve of Eratosthenes — sum of primes up to N.
 * Adapted from the canonical OCaml form which uses lazy streams; this
 * version uses plain list filtering. *)

let rec range_aux acc lo hi =
  if hi < lo then acc
  else range_aux (hi :: acc) lo (hi - 1)
let range lo hi = range_aux [] lo hi

let rec filter p lst =
  match lst with
  | [] -> []
  | h :: t -> if p h then h :: filter p t else filter p t

let rec sieve lst =
  match lst with
  | [] -> []
  | p :: t ->
      p :: sieve (filter (fun n -> n mod p <> 0) t)

let rec sum_aux acc lst =
  match lst with
  | [] -> acc
  | h :: t -> sum_aux (acc + h) t

(* Run several times to push runtime past ~1 s. *)
let rec rep n =
  if n = 0 then 0
  else
    let s = sum_aux 0 (sieve (range 2 8000)) in
    let _ = rep (n - 1) in
    s

let _ = print_int (rep 8); print_newline ()
