(* Stdlib containers: Hashtbl / Stack / Queue / Buffer *)

(* Hashtbl *)
let h = Hashtbl.create 16
let _ = Hashtbl.add h "alpha" 1
let _ = Hashtbl.add h "beta" 2
let _ = Hashtbl.add h "gamma" 3
let _ = print_int (Hashtbl.find h "alpha"); print_newline ()
let _ = print_int (Hashtbl.find h "beta"); print_newline ()
let _ = print_int (Hashtbl.length h); print_newline ()
let _ = print_endline (if Hashtbl.mem h "alpha" then "T" else "F")
let _ = print_endline (if Hashtbl.mem h "delta" then "T" else "F")
let _ = Hashtbl.remove h "alpha"
let _ = print_endline (if Hashtbl.mem h "alpha" then "T" else "F")
let _ = print_int (Hashtbl.length h); print_newline ()

(* Hashtbl.iter *)
let _ = Hashtbl.add h "alpha" 100
let _ = Hashtbl.iter (fun k v -> print_string k; print_string "="; print_int v; print_string " ") h
let _ = print_newline ()

(* Stack *)
let s = Stack.create ()
let _ = Stack.push 10 s; Stack.push 20 s; Stack.push 30 s
let _ = print_int (Stack.length s); print_newline ()
let _ = print_int (Stack.top s); print_newline ()
let _ = print_int (Stack.pop s); print_newline ()
let _ = print_int (Stack.pop s); print_newline ()
let _ = print_endline (if Stack.is_empty s then "T" else "F")
let _ = print_int (Stack.pop s); print_newline ()
let _ = print_endline (if Stack.is_empty s then "T" else "F")

(* Queue *)
let q = Queue.create ()
let _ = Queue.add 1 q; Queue.add 2 q; Queue.add 3 q
let _ = print_int (Queue.length q); print_newline ()
let _ = print_int (Queue.pop q); print_newline ()
let _ = print_int (Queue.pop q); print_newline ()
let _ = print_int (Queue.pop q); print_newline ()
let _ = print_endline (if Queue.is_empty q then "T" else "F")

(* Buffer *)
let b = Buffer.create 16
let _ = Buffer.add_string b "hello, "
let _ = Buffer.add_string b "world"
let _ = Buffer.add_char b 33    (* '!' *)
let _ = print_endline (Buffer.contents b)
let _ = print_int (Buffer.length b); print_newline ()
let _ = Buffer.clear b
let _ = print_int (Buffer.length b); print_newline ()
