;; fib(n) computed iteratively, with each result printed via env.log_i32.
;; Demonstrates imports + iterative loop.
(module
  (import "env" "log_i32" (func $log (param i32)))

  (func $fibs (export "fibs") (param $n i32)
    (local $i i32)   ;; loop counter
    (local $a i32)   ;; fib(i-2)
    (local $b i32)   ;; fib(i-1)
    (local $t i32)   ;; tmp
    (local.set $a (i32.const 0))
    (local.set $b (i32.const 1))
    (loop $body
      (if (i32.lt_s (local.get $i) (local.get $n))
        (then
          (call $log (local.get $a))
          (local.set $t (i32.add (local.get $a) (local.get $b)))
          (local.set $a (local.get $b))
          (local.set $b (local.get $t))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $body))))))
