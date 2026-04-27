;; Bitwise and shifts: count the set bits of n via popcnt, then check
;; via a manual loop with shifts/AND.
(module
  (func $popcnt_native (export "popcnt") (param $n i32) (result i32)
    (i32.popcnt (local.get $n)))

  (func $popcnt_loop (export "popcnt_loop") (param $n i32) (result i32)
    (local $count i32)
    (loop $body
      (if (i32.eqz (local.get $n))
        (then)
        (else
          (local.set $count
            (i32.add (local.get $count)
              (i32.and (local.get $n) (i32.const 1))))
          (local.set $n (i32.shr_u (local.get $n) (i32.const 1)))
          (br $body))))
    (local.get $count)))
