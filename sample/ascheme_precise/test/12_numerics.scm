;; bignum / rational / complex / multiple values
(display (expt 2 64)) (newline)
(display (expt 10 50)) (newline)
(display (* 999999999999 999999999999)) (newline)

;; factorial of 30 — comfortably bignum
(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
(display (fact 30)) (newline)

;; rational arithmetic
(display (/ 1 3)) (newline)
(display (+ 1/2 1/3)) (newline)
(display (* 2/3 3/4)) (newline)
(display (- 1/2 1/3)) (newline)
(display (numerator 3/7)) (newline)
(display (denominator 3/7)) (newline)
(display (exact? 1/3)) (newline)
(display (rational? 1/3)) (newline)         ; rational is just a real
(display (= 1/2 0.5)) (newline)              ; mixed exact/inexact compare

;; complex
(display (make-rectangular 3 4)) (newline)
(display (real-part (make-rectangular 3 4))) (newline)
(display (imag-part (make-rectangular 3 4))) (newline)
(display (magnitude (make-rectangular 3 4))) (newline)
(display (* (make-rectangular 0 1) (make-rectangular 0 1))) (newline)  ; i*i = -1
(display (+ 1 (make-rectangular 0 2))) (newline)

;; exact <-> inexact
(display (exact->inexact 1/4)) (newline)
(display (inexact->exact 1.5)) (newline)
(display (inexact->exact 3.0)) (newline)

;; gcd / lcm
(display (gcd 12 18)) (newline)
(display (gcd 36 24 16)) (newline)
(display (lcm 4 6)) (newline)
(display (lcm 4 6 9)) (newline)

;; multiple values
(call-with-values
  (lambda () (values 1 2 3))
  (lambda (a b c) (display (+ a b c)) (newline)))
(call-with-values
  (lambda () (values))
  (lambda () (display 'no-args) (newline)))
(call-with-values
  (lambda () 42)             ; producer returning single value
  (lambda (x) (display x) (newline)))
