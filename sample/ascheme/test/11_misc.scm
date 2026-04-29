;; miscellaneous
(display (number? 42)) (newline)
(display (integer? 1.0)) (newline)        ; whole-valued real → #t
(display (integer? 1.5)) (newline)
(display (exact? 1)) (newline)
(display (inexact? 1.0)) (newline)
(display (exact->inexact 1)) (newline)
(display (inexact->exact 3.0)) (newline)
(display (sqrt 16)) (newline)
(display (floor 3.7)) (newline)
(display (ceiling 3.2)) (newline)
(display (round 3.5)) (newline)
(display (round 2.5)) (newline)

;; string + symbol round trips
(display (string->symbol (symbol->string 'cat))) (newline)

;; nested let / shadowing
(define x 10)
(display (let ((x 1)) (let ((x 2)) (+ x x)))) (newline)
(display x) (newline)

;; rest args
(define (sum . args)
  (let loop ((lst args) (acc 0))
    (if (null? lst) acc (loop (cdr lst) (+ acc (car lst))))))
(display (sum 1 2 3 4 5)) (newline)

;; complicated quoted
(display '(a (b (c d)) e)) (newline)
(display (length '(a (b (c d)) e))) (newline)

;; comparison predicates
(display (zero? 0.0)) (newline)
(display (positive? -1)) (newline)
(display (negative? 0)) (newline)
