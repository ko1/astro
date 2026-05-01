(* Class inheritance + initializer.
 *
 * Notes:
 *   - `inherit X args` runs the parent constructor and merges its
 *     methods/fields into the new object.
 *   - Inherited fields aren't visible by bare name in subclass method
 *     bodies (our pre-scan only sees own fields); use `self#field`.
 *   - `initializer body` runs after the object is built, with `self`
 *     bound to the freshly created instance. *)

class animal name = object
  val mutable n = name
  method name = n
  method speak = print_endline (n ^ " makes a sound")
end

class dog name = object
  inherit animal name
  method speak = print_endline (self#name ^ " barks")
  method fetch = print_endline (self#name ^ " fetches")
end

let a = new animal "cat"
let _ = a#speak
let _ = print_endline a#name

let d = new dog "rex"
let _ = d#speak
let _ = d#fetch
let _ = print_endline d#name

class counter init_v = object (self)
  val mutable v = init_v
  method get = v
  method bump = v <- v + 1
  initializer
    print_endline "counter created"
end

let c = new counter 10
let _ = print_int c#get; print_newline ()
let _ = c#bump; c#bump; c#bump
let _ = print_int c#get; print_newline ()
