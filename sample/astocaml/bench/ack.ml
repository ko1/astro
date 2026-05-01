(* Ackermann function (3, 9) = 4093. Heavy on stack depth and recursion. *)

let rec ack m n =
  if m = 0 then n + 1
  else if n = 0 then ack (m - 1) 1
  else ack (m - 1) (ack m (n - 1))

let _ = print_int (ack 3 9); print_newline ()
