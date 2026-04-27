;; i64 version of fib — exercises 64-bit integer ops.
;; fib(40) = 102334155 (still well within i64).
(module
  (func $fib (export "fib") (param $n i64) (result i64)
    (if (result i64) (i64.lt_s (local.get $n) (i64.const 2))
      (then (local.get $n))
      (else
        (i64.add
          (call $fib (i64.sub (local.get $n) (i64.const 1)))
          (call $fib (i64.sub (local.get $n) (i64.const 2))))))))
