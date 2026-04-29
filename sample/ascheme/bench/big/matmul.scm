;; Square-matrix multiplication.  300×300 fixnum matrix, 27M multiplies.
;; Stresses nested loop + vector-of-vector indexing + specialized arith.

(define (make-matrix n)
  (let ((m (make-vector n)))
    (let loop ((i 0))
      (if (< i n)
          (begin (vector-set! m i (make-vector n 0)) (loop (+ i 1)))))
    m))

(define (fill-matrix m n seed)
  (let outer ((i 0) (s seed))
    (if (< i n)
        (let inner ((j 0) (s s))
          (if (< j n)
              (begin
                (vector-set! (vector-ref m i) j (modulo s 100))
                (inner (+ j 1) (+ (* s 1103515245) 12345)))
              (outer (+ i 1) s))))))

(define (matmul a b c n)
  (let i-loop ((i 0))
    (cond
      ((< i n)
       (let ((ai (vector-ref a i))
             (ci (vector-ref c i)))
         (let j-loop ((j 0))
           (cond
             ((< j n)
              (let k-loop ((k 0) (sum 0))
                (cond
                  ((< k n)
                   (k-loop (+ k 1)
                           (+ sum (* (vector-ref ai k)
                                     (vector-ref (vector-ref b k) j)))))
                  (else (vector-set! ci j sum))))
              (j-loop (+ j 1)))))
         (i-loop (+ i 1)))))))

(define N 200)
(define A (make-matrix N))
(define B (make-matrix N))
(define C (make-matrix N))
(fill-matrix A N 7)
(fill-matrix B N 31)
(matmul A B C N)
;; print one cell so the result isn't dead code
(display (vector-ref (vector-ref C 0) 0))
(newline)
