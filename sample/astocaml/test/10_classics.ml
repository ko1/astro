(* Classic small programs *)

(* FizzBuzz 1..20 *)
let rec fizzbuzz n max =
  if n > max then ()
  else begin
    if n mod 15 = 0 then print_endline "FizzBuzz"
    else if n mod 3 = 0 then print_endline "Fizz"
    else if n mod 5 = 0 then print_endline "Buzz"
    else (print_int n; print_newline ());
    fizzbuzz (n + 1) max
  end

let _ = fizzbuzz 1 20

(* Sum of digits *)
let rec sum_digits n =
  if n = 0 then 0
  else (n mod 10) + sum_digits (n / 10)
let _ = print_int (sum_digits 12345); print_newline ()
let _ = print_int (sum_digits 999); print_newline ()

(* Reverse digits *)
let rec rev_digits_aux n acc =
  if n = 0 then acc
  else rev_digits_aux (n / 10) (acc * 10 + n mod 10)
let rev_digits n = rev_digits_aux n 0
let _ = print_int (rev_digits 12345); print_newline ()

(* Is prime *)
let rec divides_any n d =
  if d * d > n then false
  else if n mod d = 0 then true
  else divides_any n (d + 1)
let is_prime n =
  if n < 2 then false
  else not (divides_any n 2)

let rec print_primes_below n max =
  if n >= max then ()
  else begin
    if is_prime n then begin
      print_int n;
      print_string " "
    end;
    print_primes_below (n + 1) max
  end
let _ = print_primes_below 2 30; print_newline ()

(* Count vowels — using char codes *)
(* skipped: needs char support beyond what we have *)

(* Tower of Hanoi (count moves) *)
let rec hanoi n =
  if n <= 0 then 0
  else 2 * hanoi (n - 1) + 1
let _ = print_int (hanoi 10); print_newline ()
let _ = print_int (hanoi 16); print_newline ()
