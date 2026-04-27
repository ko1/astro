;; f64 version of fib — exercises floating-point arithmetic and
;; comparison nodes (f64.lt, f64.add, f64.sub, f64.const).
;; fib(30) = 832040.0
(module
  (func $fib (export "fib") (param $n f64) (result f64)
    (if (result f64) (f64.lt (local.get $n) (f64.const 2))
      (then (local.get $n))
      (else
        (f64.add
          (call $fib (f64.sub (local.get $n) (f64.const 1)))
          (call $fib (f64.sub (local.get $n) (f64.const 2))))))))
