(* N-Queens count — adapted from the classic OCaml solution.
 * Counts the number of ways to place N non-attacking queens on an N×N board.
 * 8-queens has 92 solutions; 9-queens has 352; 10-queens has 724.
 *
 * The two functions are mutually recursive; OCaml's `and` keyword would
 * normally be used, but our subset omits it.  Mutual recursion still works
 * via global lookup — both definitions resolve at call time, not parse
 * time, so a forward reference to `solve_step` from `count_cols` is fine
 * as long as both are defined before the first call. *)

let abs n = if n < 0 then - n else n

let rec safe_aux q queens dist =
  match queens with
  | [] -> true
  | h :: t ->
      if h = q then false
      else if abs (h - q) = dist then false
      else safe_aux q t (dist + 1)

let safe q queens = safe_aux q queens 1

let rec list_length lst =
  match lst with
  | [] -> 0
  | _ :: t -> 1 + list_length t

let rec range_aux acc n =
  if n = 0 then acc
  else range_aux (n :: acc) (n - 1)
let range n = range_aux [] n

let rec count_cols n queens cols =
  match cols with
  | [] -> 0
  | c :: rest ->
      let here = if safe c queens then solve_step n (c :: queens) else 0 in
      here + count_cols n queens rest
and solve_step n queens =
  if list_length queens = n then 1
  else count_cols n queens (range n)

(* Loop to get a sustained ~1 s reading. *)
let rec rep n acc =
  if n = 0 then acc
  else rep (n - 1) (solve_step 10 [])
let _ = print_int (rep 3 0); print_newline ()
