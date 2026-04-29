;; CPS-transformed loop — every call is in continuation-passing style,
;; so closures get exercised heavily.  Stresses lambda allocation + GC.
(define (sum-cps n k)
  (let loop ((i 0) (acc 0))
    (if (= i n) (k acc) (loop (+ i 1) (+ acc i)))))

(define (run)
  (sum-cps 1000000 (lambda (s) s)))

(let driver ((trial 0) (last 0))
  (if (= trial 50)
      (begin (display last) (newline))
      (driver (+ trial 1) (run))))
