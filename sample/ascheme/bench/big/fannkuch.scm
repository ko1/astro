;; Fannkuch — generate every permutation of 1..N (Heap's algorithm) and
;; record the maximum prefix-flip count.  Vector-heavy, exercises tight
;; integer arithmetic in the inner flip loop.
(define (count-flips perm n)
  (let ((q (make-vector n 0)))
    (let copy ((i 0))
      (if (< i n)
          (begin (vector-set! q i (vector-ref perm i)) (copy (+ i 1)))))
    (let outer ((flips 0))
      (let ((first (vector-ref q 0)))
        (if (= first 1)
            flips
            (begin
              (let inner ((i 0) (j (- first 1)))
                (if (< i j)
                    (let ((t (vector-ref q i)))
                      (vector-set! q i (vector-ref q j))
                      (vector-set! q j t)
                      (inner (+ i 1) (- j 1)))))
              (outer (+ flips 1))))))))

(define (fannkuch n)
  (define p (make-vector n 0))
  (define max-flips 0)
  (define (init i)
    (if (< i n)
        (begin (vector-set! p i (+ i 1)) (init (+ i 1)))))
  ;; Heap's algorithm: recursively generate permutations and visit each
  ;; once.  At each leaf we count flips and track the running max.
  (define (perms k)
    (cond
      ((= k 1)
       (let ((f (count-flips p n)))
         (if (> f max-flips) (set! max-flips f))))
      (else
       (let outer ((i 0))
         (cond
           ((< i k)
            (perms (- k 1))
            (let* ((swap-i (if (even? k) i 0))
                   (last (- k 1))
                   (t (vector-ref p swap-i)))
              (vector-set! p swap-i (vector-ref p last))
              (vector-set! p last t))
            (outer (+ i 1))))))))
  (init 0)
  (perms n)
  max-flips)

(display (fannkuch 9))
(newline)
