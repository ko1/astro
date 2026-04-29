;; Mandelbrot escape-count over an N×N grid.  Pure flonum arithmetic in
;; an inner loop — exercises the boxed-double path of add2/sub2/mul2.
(define (mandel-iter cx cy max)
  (let loop ((zx 0.0) (zy 0.0) (i 0))
    (cond ((= i max) max)
          ((> (+ (* zx zx) (* zy zy)) 4.0) i)
          (else
           (loop (+ (- (* zx zx) (* zy zy)) cx)
                 (+ (* 2.0 zx zy) cy)
                 (+ i 1))))))

(define (mandel size max)
  (let outer ((y 0) (sum 0))
    (if (= y size) sum
        (let inner ((x 0) (row 0))
          (if (= x size)
              (outer (+ y 1) (+ sum row))
              (let ((cx (- (* 3.5 (/ x size)) 2.5))
                    (cy (- (* 2.0 (/ y size)) 1.0)))
                (inner (+ x 1) (+ row (mandel-iter cx cy max)))))))))

(display (mandel 200 100))
(newline)
