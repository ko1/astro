;; Leibniz pi approximation — single tight f64 loop with a sign flip
;; per iteration.  Contrast with mandelbrot/spectral: only one loop,
;; no nested array indexing, no multiple state variables.  A useful
;; "hello FP" benchmark to see whether wastro keeps up with wasmtime
;; on the simplest possible f64 case.
;;
;;   pi(1000000)   -> 3141592 (6 sig figs × 1e6)
;;   pi(10000000)  -> 3141592 (same precision; convergence is slow)

(module
  (func $pi (export "pi") (param $n i32) (result i64)
    (local $sum f64) (local $sign f64) (local $i i32) (local $denom f64)
    (local.set $sign (f64.const 1.0))
    (local.set $i (i32.const 0))
    (block $end (loop $L
      (br_if $end (i32.ge_s (local.get $i) (local.get $n)))
      (local.set $denom
        (f64.add (f64.mul (f64.convert_i32_s (local.get $i)) (f64.const 2.0))
                 (f64.const 1.0)))
      (local.set $sum
        (f64.add (local.get $sum)
                 (f64.div (local.get $sign) (local.get $denom))))
      (local.set $sign (f64.neg (local.get $sign)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $L)))
    ;; result = 4 * sum × 1e6 — 6-digit integer fingerprint
    (i64.trunc_f64_s
      (f64.mul (f64.mul (local.get $sum) (f64.const 4.0))
               (f64.const 1000000.0)))))
