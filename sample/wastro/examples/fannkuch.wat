;; Fannkuch-redux — Computer Language Benchmarks Game classic.
;; Counts the maximum number of "pancake flips" needed to sort 1..N
;; over every permutation, plus a signed checksum.  Heavy on i32 array
;; indexing through linear memory and tight branchy loops.
;;
;;   fannkuch(7)  -> 16   (5040  perms)
;;   fannkuch(9)  -> 30   (362880 perms)
;;   fannkuch(10) -> 38
;;   fannkuch(11) -> 51

(module
  (memory 1)  ;; perm[], perm1[], count[] live here

  (func $fannkuch (export "fannkuch") (param $n i32) (result i32)
    (local $i i32) (local $j i32) (local $r i32)
    (local $k i32) (local $k2 i32) (local $tmp i32) (local $perm0 i32)
    (local $flips i32) (local $maxFlips i32) (local $checksum i32) (local $permCnt i32)
    (local $perm i32) (local $perm1 i32) (local $cnt i32)

    (local.set $perm  (i32.const 0))
    (local.set $perm1 (i32.shl (local.get $n) (i32.const 2)))
    (local.set $cnt   (i32.shl (local.get $n) (i32.const 3)))

    ;; perm1[i] = i
    (local.set $i (i32.const 0))
    (block $init_end (loop $init_loop
      (br_if $init_end (i32.ge_s (local.get $i) (local.get $n)))
      (i32.store
        (i32.add (local.get $perm1) (i32.shl (local.get $i) (i32.const 2)))
        (local.get $i))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $init_loop)))

    (local.set $r (local.get $n))

    (block $outer_end (loop $outer_loop
      ;; while (r != 1) { cnt[r-1] = r; r--; }
      (block $rfill_end (loop $rfill_loop
        (br_if $rfill_end (i32.eq (local.get $r) (i32.const 1)))
        (i32.store
          (i32.add (local.get $cnt) (i32.shl (i32.sub (local.get $r) (i32.const 1)) (i32.const 2)))
          (local.get $r))
        (local.set $r (i32.sub (local.get $r) (i32.const 1)))
        (br $rfill_loop)))

      ;; perm = perm1 (copy)
      (local.set $i (i32.const 0))
      (block $copy_end (loop $copy_loop
        (br_if $copy_end (i32.ge_s (local.get $i) (local.get $n)))
        (i32.store
          (i32.add (local.get $perm) (i32.shl (local.get $i) (i32.const 2)))
          (i32.load (i32.add (local.get $perm1) (i32.shl (local.get $i) (i32.const 2)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $copy_loop)))

      ;; flips = 0; while ((k = perm[0]) != 0) { reverse perm[0..k]; flips++; }
      (local.set $flips (i32.const 0))
      (block $flip_end (loop $flip_loop
        (local.set $k (i32.load (local.get $perm)))
        (br_if $flip_end (i32.eqz (local.get $k)))
        (local.set $k2 (i32.shr_u (i32.add (local.get $k) (i32.const 1)) (i32.const 1)))
        (local.set $i (i32.const 0))
        (block $swap_end (loop $swap_loop
          (br_if $swap_end (i32.ge_s (local.get $i) (local.get $k2)))
          (local.set $tmp
            (i32.load (i32.add (local.get $perm) (i32.shl (local.get $i) (i32.const 2)))))
          (i32.store
            (i32.add (local.get $perm) (i32.shl (local.get $i) (i32.const 2)))
            (i32.load (i32.add (local.get $perm)
              (i32.shl (i32.sub (local.get $k) (local.get $i)) (i32.const 2)))))
          (i32.store
            (i32.add (local.get $perm)
              (i32.shl (i32.sub (local.get $k) (local.get $i)) (i32.const 2)))
            (local.get $tmp))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $swap_loop)))
        (local.set $flips (i32.add (local.get $flips) (i32.const 1)))
        (br $flip_loop)))

      ;; track max + signed checksum
      (if (i32.gt_s (local.get $flips) (local.get $maxFlips))
        (then (local.set $maxFlips (local.get $flips))))
      (if (i32.eqz (i32.and (local.get $permCnt) (i32.const 1)))
        (then (local.set $checksum (i32.add (local.get $checksum) (local.get $flips))))
        (else (local.set $checksum (i32.sub (local.get $checksum) (local.get $flips)))))

      ;; next permutation (rotate perm1[0..r] left, count down cnt[r])
      (block $advance_end (loop $advance_loop
        (br_if $outer_end (i32.eq (local.get $r) (local.get $n)))

        (local.set $perm0 (i32.load (local.get $perm1)))
        (local.set $i (i32.const 0))
        (block $shift_end (loop $shift_loop
          (br_if $shift_end (i32.ge_s (local.get $i) (local.get $r)))
          (local.set $j (i32.add (local.get $i) (i32.const 1)))
          (i32.store
            (i32.add (local.get $perm1) (i32.shl (local.get $i) (i32.const 2)))
            (i32.load (i32.add (local.get $perm1) (i32.shl (local.get $j) (i32.const 2)))))
          (local.set $i (local.get $j))
          (br $shift_loop)))
        (i32.store
          (i32.add (local.get $perm1) (i32.shl (local.get $r) (i32.const 2)))
          (local.get $perm0))

        (i32.store
          (i32.add (local.get $cnt) (i32.shl (local.get $r) (i32.const 2)))
          (i32.sub
            (i32.load (i32.add (local.get $cnt) (i32.shl (local.get $r) (i32.const 2))))
            (i32.const 1)))
        (br_if $advance_end
          (i32.gt_s
            (i32.load (i32.add (local.get $cnt) (i32.shl (local.get $r) (i32.const 2))))
            (i32.const 0)))
        (local.set $r (i32.add (local.get $r) (i32.const 1)))
        (br $advance_loop)))

      (local.set $permCnt (i32.add (local.get $permCnt) (i32.const 1)))
      (br $outer_loop)))

    (local.get $maxFlips)))
