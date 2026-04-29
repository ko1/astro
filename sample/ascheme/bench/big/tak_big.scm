;; takeuchi (32 22 10) — well-known recursion benchmark.  Each call site
;; nests three more `tak` calls; not tail-recursive, so the C stack
;; really gets exercised.
(define (tak x y z)
  (if (not (< y x))
      z
      (tak (tak (- x 1) y z)
           (tak (- y 1) z x)
           (tak (- z 1) x y))))
(display (tak 32 22 10))
(newline)
