;; lambda, let, letrec, closures
(display ((lambda (x y) (+ x y)) 3 4)) (newline)
(display ((lambda x x) 1 2 3)) (newline)             ; rest only
(display ((lambda (a b . rest) (list a b rest)) 1 2 3 4 5)) (newline)
(display (let ((x 5) (y 7)) (+ x y))) (newline)
(display (let* ((x 5) (y (+ x 1))) (* x y))) (newline)
(display (letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1)))))
                  (odd?  (lambda (n) (if (= n 0) #f (even? (- n 1))))))
           (list (even? 10) (odd? 7)))) (newline)
;; closures
(define (make-counter)
  (let ((n 0))
    (lambda ()
      (set! n (+ n 1))
      n)))
(define c1 (make-counter))
(display (c1)) (newline)
(display (c1)) (newline)
(display (c1)) (newline)

;; higher-order
(define (twice f) (lambda (x) (f (f x))))
(display ((twice (lambda (x) (* x 2))) 5)) (newline)

;; internal define
(define (inner)
  (define x 1)
  (define y 2)
  (+ x y))
(display (inner)) (newline)

;; mutual recursion via letrec
(display (letrec ((sum (lambda (n) (if (= n 0) 0 (+ n (sum (- n 1)))))))
           (sum 100))) (newline)
