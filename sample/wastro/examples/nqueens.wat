;; N-Queens — count distinct solutions on N×N board via bit-mask
;; recursion.  No memory, no FP — i32 bitwise + deep recursive
;; backtracking.  Stresses both call cost and tight branchy code.
;;
;;   queens(8)  -> 92
;;   queens(10) -> 724
;;   queens(11) -> 2680
;;   queens(12) -> 14200
;;   queens(13) -> 73712

(module
  ;; solve(cols, d1, d2, all) — count placements that fill every column.
  ;;   cols    bitmask of occupied columns
  ;;   d1, d2  bitmasks of attacked diagonals (shifted each level)
  ;;   all     (1 << n) - 1  — all columns filled sentinel
  (func $solve (param $cols i32) (param $d1 i32) (param $d2 i32) (param $all i32) (result i32)
    (local $count i32)
    (local $available i32)
    (local $bit i32)

    (if (result i32) (i32.eq (local.get $cols) (local.get $all))
      (then (i32.const 1))
      (else
        ;; available = ~(cols | d1 | d2) & all  — squares safe to place
        (local.set $available
          (i32.and
            (i32.xor
              (i32.or (i32.or (local.get $cols) (local.get $d1)) (local.get $d2))
              (i32.const -1))
            (local.get $all)))
        (block $done
          (loop $L
            (br_if $done (i32.eqz (local.get $available)))
            ;; bit = available & -available  — lowest set bit
            (local.set $bit
              (i32.and (local.get $available)
                       (i32.sub (i32.const 0) (local.get $available))))
            (local.set $available (i32.sub (local.get $available) (local.get $bit)))
            (local.set $count
              (i32.add (local.get $count)
                (call $solve
                  (i32.or (local.get $cols) (local.get $bit))
                  (i32.shl (i32.or (local.get $d1) (local.get $bit)) (i32.const 1))
                  (i32.shr_u (i32.or (local.get $d2) (local.get $bit)) (i32.const 1))
                  (local.get $all))))
            (br $L)))
        (local.get $count))))

  (func $queens (export "queens") (param $n i32) (result i32)
    (call $solve
      (i32.const 0) (i32.const 0) (i32.const 0)
      (i32.sub (i32.shl (i32.const 1) (local.get $n)) (i32.const 1)))))
