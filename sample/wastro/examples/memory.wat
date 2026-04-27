;; Memory smoke test: store, load, sum.
;;   Writes the values 0..n-1 to memory[0..4n] (i32 each), then sums them.
(module
  (memory 1 16)   ;; 1 page initial, 16 pages max (= 1 MB)

  (func $sum_writes (export "sum_writes") (param $n i32) (result i32)
    (local $i i32)
    (local $sum i32)
    ;; Phase 1: write 0..n-1 to mem[0..4n]
    (loop $write
      (if (i32.lt_s (local.get $i) (local.get $n))
        (then
          (i32.store
            (i32.shl (local.get $i) (i32.const 2))    ;; addr = i * 4
            (local.get $i))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $write))))
    ;; Phase 2: read them back and sum
    (local.set $i (i32.const 0))
    (loop $read
      (if (i32.lt_s (local.get $i) (local.get $n))
        (then
          (local.set $sum
            (i32.add (local.get $sum)
              (i32.load (i32.shl (local.get $i) (i32.const 2)))))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $read))))
    (local.get $sum)))
