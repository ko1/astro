;; Smoke test for --profile: run a small recursion and verify the
;; resulting profile.txt records the body's dispatch count.  We invoke
;; this script directly (no special CLI needed); it just exercises the
;; counter increments.
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(display (fib 15)) (newline)
;; Rest of the file uses defines to ensure they don't accidentally
;; survive past --use-profile threshold filtering when no profile.txt
;; exists yet.
(define (mul-add a b c) (+ (* a b) c))
(display (mul-add 3 4 5)) (newline)
