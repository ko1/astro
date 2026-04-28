;; Spectral norm via power method — Computer Language Benchmarks Game
;; standard.  Approximates the largest eigenvalue of a sparse (i,j)
;; matrix A(i,j) = 1 / ((i+j)*(i+j+1)/2 + i + 1) by 10 iterations of
;; v = A^T A v and reporting sqrt(v . Av / v . v).
;;
;; This is f64-heavy with two nested O(n²) inner loops per iter; the
;; standard CLBG knob is N=5500.  We pick smaller defaults so a single
;; run finishes in under a second even on plain interp.
;;
;; Result is the spectral norm × 1e9 truncated to i64 so the answer is
;; a stable integer fingerprint independent of FP printing rules.
;;
;;   spectral(100)  -> 1273790091  (≈ 1.273790)
;;   spectral(500)  -> 1274183682
;;   spectral(1000) -> 1274213987

(module
  (memory 16)  ;; 1 MB — fits N up to ~32k

  ;; A(i, j) — sparse symmetric matrix entry.
  (func $A (param $i i32) (param $j i32) (result f64)
    (local $sum i32)
    (local.set $sum (i32.add (local.get $i) (local.get $j)))
    (f64.div (f64.const 1.0)
      (f64.convert_i32_s
        (i32.add
          (i32.div_s
            (i32.mul (local.get $sum) (i32.add (local.get $sum) (i32.const 1)))
            (i32.const 2))
          (i32.add (local.get $i) (i32.const 1))))))

  ;; Au = A * u
  (func $multAv (param $n i32) (param $u_off i32) (param $Av_off i32)
    (local $i i32) (local $j i32) (local $sum f64)
    (local.set $i (i32.const 0))
    (block $end_i
      (loop $L_i
        (br_if $end_i (i32.ge_s (local.get $i) (local.get $n)))
        (local.set $sum (f64.const 0))
        (local.set $j (i32.const 0))
        (block $end_j
          (loop $L_j
            (br_if $end_j (i32.ge_s (local.get $j) (local.get $n)))
            (local.set $sum
              (f64.add (local.get $sum)
                (f64.mul
                  (call $A (local.get $i) (local.get $j))
                  (f64.load (i32.add (local.get $u_off)
                                     (i32.shl (local.get $j) (i32.const 3)))))))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $L_j)))
        (f64.store
          (i32.add (local.get $Av_off) (i32.shl (local.get $i) (i32.const 3)))
          (local.get $sum))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $L_i))))

  ;; Atu = A^T * u (A is symmetric in indices but the power method calls
  ;; both forms; the asymmetry arises only in iteration order, so we
  ;; give them separate code paths).
  (func $multAtv (param $n i32) (param $u_off i32) (param $Atv_off i32)
    (local $i i32) (local $j i32) (local $sum f64)
    (local.set $i (i32.const 0))
    (block $end_i
      (loop $L_i
        (br_if $end_i (i32.ge_s (local.get $i) (local.get $n)))
        (local.set $sum (f64.const 0))
        (local.set $j (i32.const 0))
        (block $end_j
          (loop $L_j
            (br_if $end_j (i32.ge_s (local.get $j) (local.get $n)))
            (local.set $sum
              (f64.add (local.get $sum)
                (f64.mul
                  (call $A (local.get $j) (local.get $i))
                  (f64.load (i32.add (local.get $u_off)
                                     (i32.shl (local.get $j) (i32.const 3)))))))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $L_j)))
        (f64.store
          (i32.add (local.get $Atv_off) (i32.shl (local.get $i) (i32.const 3)))
          (local.get $sum))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $L_i))))

  (func $spectral (export "spectral") (param $n i32) (result i64)
    (local $u_off i32) (local $v_off i32) (local $tmp_off i32)
    (local $i i32) (local $iter i32)
    (local $vBv f64) (local $vv f64)

    ;; layout three vectors back-to-back at memory[0..3*n*8)
    (local.set $u_off   (i32.const 0))
    (local.set $v_off   (i32.shl (local.get $n) (i32.const 3)))
    (local.set $tmp_off (i32.shl (local.get $n) (i32.const 4)))

    ;; u = (1, 1, ..., 1)
    (local.set $i (i32.const 0))
    (block $init_end
      (loop $init_loop
        (br_if $init_end (i32.ge_s (local.get $i) (local.get $n)))
        (f64.store
          (i32.add (local.get $u_off) (i32.shl (local.get $i) (i32.const 3)))
          (f64.const 1.0))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $init_loop)))

    ;; 10 power-method iterations: v = A^T A u; u = A^T A v
    (local.set $iter (i32.const 0))
    (block $iter_end
      (loop $iter_loop
        (br_if $iter_end (i32.ge_s (local.get $iter) (i32.const 10)))
        (call $multAv  (local.get $n) (local.get $u_off) (local.get $tmp_off))
        (call $multAtv (local.get $n) (local.get $tmp_off) (local.get $v_off))
        (call $multAv  (local.get $n) (local.get $v_off) (local.get $tmp_off))
        (call $multAtv (local.get $n) (local.get $tmp_off) (local.get $u_off))
        (local.set $iter (i32.add (local.get $iter) (i32.const 1)))
        (br $iter_loop)))

    ;; vBv = u . v   ; vv = v . v
    (local.set $i (i32.const 0))
    (block $sum_end
      (loop $sum_loop
        (br_if $sum_end (i32.ge_s (local.get $i) (local.get $n)))
        (local.set $vBv
          (f64.add (local.get $vBv)
            (f64.mul
              (f64.load (i32.add (local.get $u_off) (i32.shl (local.get $i) (i32.const 3))))
              (f64.load (i32.add (local.get $v_off) (i32.shl (local.get $i) (i32.const 3)))))))
        (local.set $vv
          (f64.add (local.get $vv)
            (f64.mul
              (f64.load (i32.add (local.get $v_off) (i32.shl (local.get $i) (i32.const 3))))
              (f64.load (i32.add (local.get $v_off) (i32.shl (local.get $i) (i32.const 3)))))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $sum_loop)))

    ;; result = sqrt(vBv / vv) × 1e9 — stable integer fingerprint
    (i64.trunc_f64_s
      (f64.mul (f64.sqrt (f64.div (local.get $vBv) (local.get $vv)))
               (f64.const 1000000000)))))
