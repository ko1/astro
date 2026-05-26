;; vector_closures regression test (= fuzzer found bug fixed by node_loop per-iter
;; fresh-frame allocation when body has inner lambda).
;; Each lambda captures the i at its creation; R5RS semantics require fresh
;; per-iter binding so all 5 closures see distinct i values.
(define v (make-vector 5 0))
(define (init i)
  (if (= i 5) (quote done)
      (begin (vector-set! v i (lambda () (* i i))) (init (+ i 1)))))
(init 0)
(define (sum i acc)
  (if (= i 5) acc (sum (+ i 1) (+ acc ((vector-ref v i))))))
(display (sum 0 0)) (newline)
