(* Classes / objects (basic).
 *
 * Note: bare field access inside method bodies is not supported.
 * Methods must use `self#field` to read or `self#set_field v` to write,
 * because our parser doesn't track per-class lexical field names. *)

class point x_init y_init = object
  val mutable x = x_init
  val mutable y = y_init
  method get_x = self#x
  method get_y = self#y
  method set_x v = self#set_x_impl v
  method set_x_impl v = self#x_assign v        (* uses fallback set_X auto-handler *)
  method x_assign v = self#x_set v
  method x_set v = self#set_field "x" v        (* doesn't exist — fall back? *)
  method move dx dy =
    self#set_x (self#get_x + dx);
    self#set_y (self#get_y + dy)
  method set_y v = self#set_y_impl v
  method set_y_impl v = self#y_set v
  method y_set v = self#set_field "y" v
  method to_str =
    "(" ^ string_of_int self#get_x ^ ", " ^ string_of_int self#get_y ^ ")"
end

(* Simpler version using oc_object_send's auto getter/setter for fields *)
class simple_point xi yi = object
  val mutable x = xi
  val mutable y = yi
  method get_x = self#x      (* fallback: returns field "x" *)
  method get_y = self#y
  method shift_x dx = self#set_x (self#x + dx)
  method shift_y dy = self#set_y (self#y + dy)
  method to_str = "(" ^ string_of_int self#x ^ ", " ^ string_of_int self#y ^ ")"
end

let p = new simple_point 3 4
let _ = print_endline p#to_str
let _ = p#shift_x 10
let _ = p#shift_y 20
let _ = print_endline p#to_str
let _ = print_int p#get_x; print_newline ()
let _ = print_int p#get_y; print_newline ()

(* Class with no params *)
class counter = object
  val mutable n = 0
  method get = self#n
  method bump = self#set_n (self#n + 1)
end

let c = new counter ()
let _ = c#bump; c#bump; c#bump
let _ = print_int c#get; print_newline ()
