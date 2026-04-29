;; fib(35) — exponential recursion, 30x heavier than fib(30).
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(display (fib 35))
(newline)
