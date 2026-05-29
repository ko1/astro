(* A long tail-recursive loop: 2,000,000 iterations.  This only completes in
   O(1) C stack thanks to tail-call elimination (the trampoline in ac_apply);
   without it the interpreter would overflow the stack.  The result exceeds
   int32, so it also exercises the 63-bit tagged int path. *)
let rec loop i acc = if i <= 0 then acc else loop (i - 1) (acc + i) in
print_int (loop 2000000 0);
print_newline ()
