;; Ackermann function — classic deep-recursion benchmark.  No memory
;; ops, no FP — pure i32 recursion, hammer-tests call setup cost.
;;
;;   ack(3, 8)  -> 2045
;;   ack(3, 10) -> 8189
;;   ack(3, 11) -> 16381
;;   ack(3, 12) -> 32765   (~10s on plain interp)

(module
  (func $ack (export "ack") (param $m i32) (param $n i32) (result i32)
    (if (result i32) (i32.eqz (local.get $m))
      (then (i32.add (local.get $n) (i32.const 1)))
      (else (if (result i32) (i32.eqz (local.get $n))
        (then (call $ack (i32.sub (local.get $m) (i32.const 1)) (i32.const 1)))
        (else (call $ack
          (i32.sub (local.get $m) (i32.const 1))
          (call $ack (local.get $m) (i32.sub (local.get $n) (i32.const 1))))))))))
