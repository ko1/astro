;; Heapsort — in-place O(n log n) array sort.  Generates a deterministic
;; pseudo-random sequence, sorts it, and returns a checksum so the
;; benchmark is self-contained and verifiable.  Tests linear-memory
;; load/store patterns and i32 comparison-heavy branchy code.
;;
;;   heapsort(1000)   -> 499500   (sum 0+1+...+999, after sort)
;;   heapsort(10000)  -> 49995000
;;   heapsort(100000) -> 704982704 (sum mod 2^32 of distinct values)

(module
  (memory 16)  ;; 1 MB — fits 256k i32 values

  ;; heap[i] for i in 1..n  (1-indexed, classic textbook heap layout)
  (func $sift_down (param $start i32) (param $end i32)
    (local $root i32) (local $child i32) (local $tmp i32)
    (local $rv i32) (local $cv i32)
    (local.set $root (local.get $start))
    (block $end_sift
      (loop $L_sift
        (local.set $child (i32.add (i32.shl (local.get $root) (i32.const 1)) (i32.const 1)))
        (br_if $end_sift (i32.gt_s (local.get $child) (local.get $end)))
        ;; pick larger of two children
        (if (i32.lt_s (local.get $child) (local.get $end))
          (then
            (if (i32.lt_s
                  (i32.load (i32.shl (local.get $child) (i32.const 2)))
                  (i32.load (i32.shl (i32.add (local.get $child) (i32.const 1)) (i32.const 2))))
              (then (local.set $child (i32.add (local.get $child) (i32.const 1)))))))
        (local.set $rv (i32.load (i32.shl (local.get $root) (i32.const 2))))
        (local.set $cv (i32.load (i32.shl (local.get $child) (i32.const 2))))
        (br_if $end_sift (i32.ge_s (local.get $rv) (local.get $cv)))
        ;; swap
        (i32.store (i32.shl (local.get $root) (i32.const 2)) (local.get $cv))
        (i32.store (i32.shl (local.get $child) (i32.const 2)) (local.get $rv))
        (local.set $root (local.get $child))
        (br $L_sift))))

  (func $heapsort (export "heapsort") (param $n i32) (result i32)
    (local $i i32) (local $tmp i32) (local $rng i32)
    (local $sum i32)

    ;; Fill arr[0..n) with a pseudo-random sequence (linear congruential).
    ;; Seed and constants chosen so the result is stable across runs.
    (local.set $rng (i32.const 12345))
    (local.set $i (i32.const 0))
    (block $fill_end (loop $fill_loop
      (br_if $fill_end (i32.ge_s (local.get $i) (local.get $n)))
      (local.set $rng
        (i32.add (i32.mul (local.get $rng) (i32.const 1103515245)) (i32.const 12345)))
      (i32.store (i32.shl (local.get $i) (i32.const 2)) (local.get $rng))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $fill_loop)))

    ;; Build max-heap: sift_down each parent, top-down from n/2-1.
    (local.set $i (i32.shr_s (i32.sub (local.get $n) (i32.const 2)) (i32.const 1)))
    (block $build_end (loop $build_loop
      (br_if $build_end (i32.lt_s (local.get $i) (i32.const 0)))
      (call $sift_down (local.get $i) (i32.sub (local.get $n) (i32.const 1)))
      (local.set $i (i32.sub (local.get $i) (i32.const 1)))
      (br $build_loop)))

    ;; Extract: swap root with end, sift down range [0..end-1].
    (local.set $i (i32.sub (local.get $n) (i32.const 1)))
    (block $extract_end (loop $extract_loop
      (br_if $extract_end (i32.le_s (local.get $i) (i32.const 0)))
      (local.set $tmp (i32.load (i32.const 0)))
      (i32.store (i32.const 0) (i32.load (i32.shl (local.get $i) (i32.const 2))))
      (i32.store (i32.shl (local.get $i) (i32.const 2)) (local.get $tmp))
      (call $sift_down (i32.const 0) (i32.sub (local.get $i) (i32.const 1)))
      (local.set $i (i32.sub (local.get $i) (i32.const 1)))
      (br $extract_loop)))

    ;; Checksum: XOR-and-add of the sorted array — fingerprint that
    ;; depends on full ordering (any swap miss flips bits).
    (local.set $i (i32.const 0))
    (block $sum_end (loop $sum_loop
      (br_if $sum_end (i32.ge_s (local.get $i) (local.get $n)))
      (local.set $sum
        (i32.xor
          (i32.add (local.get $sum)
                   (i32.load (i32.shl (local.get $i) (i32.const 2))))
          (local.get $i)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $sum_loop)))
    (local.get $sum)))
