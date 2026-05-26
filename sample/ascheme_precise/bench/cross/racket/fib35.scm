#lang racket/base
(require racket/math)  ; for abs etc.
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(display (fib 35)) (newline)
