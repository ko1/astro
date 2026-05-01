(* Method-send heavy benchmark.  A counter object whose `incr` method
   is called in a tight loop — exercises the inline cache on `obj#m`. *)

class counter init = object
  val mutable n = init
  method incr () = n <- n + 1
  method incr_by k = n <- n + k
  method get () = n
end

let c = new counter 0
let n = 5000000

let rec loop1 i =
  if i = 0 then ()
  else begin c#incr (); loop1 (i - 1) end

let _ = loop1 n
let _ = print_int (c#get ()); print_newline ()

let c2 = new counter 0
let rec loop2 i =
  if i = 0 then ()
  else begin c2#incr_by 2; loop2 (i - 1) end

let _ = loop2 n
let _ = print_int (c2#get ()); print_newline ()
