;; Same iterative sum, but with `br_if` directly to the loop label
;; (continue) instead of an outer `block + br`.  Idiomatic wasm
;; structure for "while" loops.
(module
  (func $sum (export "sum") (param $n i32) (result i32)
    (local $i i32)
    (local $s i32)
    (loop $body
      (if (i32.lt_s (local.get $i) (local.get $n))
        (then
          (local.set $s (i32.add (local.get $s) (local.get $i)))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $body))))
    (local.get $s)))
