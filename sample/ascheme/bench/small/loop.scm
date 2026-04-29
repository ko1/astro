;; tight tail-call loop — exercises TCO trampoline
(define (loop n)
  (if (= n 0) 'done (loop (- n 1))))
(display (loop 25000000))
(newline)
