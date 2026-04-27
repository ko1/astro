;; Demonstrates `return` for early exit.
;; abs(n) — returns -n if n < 0, else n.
(module
  (func $abs (export "abs") (param $n i32) (result i32)
    (if (i32.lt_s (local.get $n) (i32.const 0))
      (then (return (i32.sub (i32.const 0) (local.get $n)))))
    (local.get $n)))
