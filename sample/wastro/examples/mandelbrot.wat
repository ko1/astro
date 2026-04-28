;; Mandelbrot escape-iteration counter.
;;
;; Iterates over an N×N grid in the complex plane (x in [-2, 1], y in
;; [-1.5, 1.5]) and counts how many points escape after at most 100
;; iterations.  Standard FP-heavy benchmark.
;;
;;   mandel(N) -> number of escaped points (sanity-check: depends on N)
;;     mandel(100)  -> 7028
;;     mandel(500)  -> 175940
;;     mandel(1000) -> 705294

(module
  (func $mandel (export "mandel") (param $N i32) (result i32)
    (local $iy i32)
    (local $ix i32)
    (local $cx f64)
    (local $cy f64)
    (local $zx f64)
    (local $zy f64)
    (local $zx2 f64)
    (local $zy2 f64)
    (local $iter i32)
    (local $escaped i32)
    (local $maxIter i32)
    (local.set $maxIter (i32.const 100))

    (block $end_y
      (loop $loop_y
        (br_if $end_y (i32.ge_s (local.get $iy) (local.get $N)))

        (local.set $ix (i32.const 0))
        (block $end_x
          (loop $loop_x
            (br_if $end_x (i32.ge_s (local.get $ix) (local.get $N)))

            ;; cx = -2 + 3 * ix / N
            (local.set $cx
              (f64.add (f64.const -2.0)
                (f64.div
                  (f64.mul (f64.const 3.0)
                    (f64.convert_i32_s (local.get $ix)))
                  (f64.convert_i32_s (local.get $N)))))
            ;; cy = -1.5 + 3 * iy / N
            (local.set $cy
              (f64.add (f64.const -1.5)
                (f64.div
                  (f64.mul (f64.const 3.0)
                    (f64.convert_i32_s (local.get $iy)))
                  (f64.convert_i32_s (local.get $N)))))

            (local.set $zx (f64.const 0.0))
            (local.set $zy (f64.const 0.0))
            (local.set $iter (i32.const 0))

            (block $end_iter
              (loop $loop_iter
                (br_if $end_iter (i32.ge_s (local.get $iter) (local.get $maxIter)))
                (local.set $zx2 (f64.mul (local.get $zx) (local.get $zx)))
                (local.set $zy2 (f64.mul (local.get $zy) (local.get $zy)))
                (br_if $end_iter
                  (f64.gt
                    (f64.add (local.get $zx2) (local.get $zy2))
                    (f64.const 4.0)))
                ;; (zx, zy) = (zx*zx - zy*zy + cx, 2*zx*zy + cy)
                (local.set $zy
                  (f64.add
                    (f64.mul (f64.const 2.0)
                      (f64.mul (local.get $zx) (local.get $zy)))
                    (local.get $cy)))
                (local.set $zx
                  (f64.add
                    (f64.sub (local.get $zx2) (local.get $zy2))
                    (local.get $cx)))
                (local.set $iter (i32.add (local.get $iter) (i32.const 1)))
                (br $loop_iter)))

            (if (i32.lt_s (local.get $iter) (local.get $maxIter))
              (then
                (local.set $escaped (i32.add (local.get $escaped) (i32.const 1)))))

            (local.set $ix (i32.add (local.get $ix) (i32.const 1)))
            (br $loop_x)))

        (local.set $iy (i32.add (local.get $iy) (i32.const 1)))
        (br $loop_y)))
    (local.get $escaped)))
