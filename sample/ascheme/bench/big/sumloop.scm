;; 100M-step tail-call loop (50× the small one).  Stresses the trampoline
;; + specialized arith on hot-path TCO; result needs bignum for the sum.
(display
  (let loop ((i 0) (s 0))
    (if (= i 100000000) s (loop (+ i 1) (+ s i)))))
(newline)
