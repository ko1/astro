;; classic R5RS samples
(define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
(display (fib 20)) (newline)

(define (ack m n)
  (cond ((= m 0) (+ n 1))
        ((= n 0) (ack (- m 1) 1))
        (else (ack (- m 1) (ack m (- n 1))))))
(display (ack 3 3)) (newline)

(define (tarai x y z)
  (if (<= x y) y
      (tarai (tarai (- x 1) y z)
             (tarai (- y 1) z x)
             (tarai (- z 1) x y))))
(display (tarai 8 4 0)) (newline)

;; sieve of eratosthenes
(define (range a b)
  (if (>= a b) '() (cons a (range (+ a 1) b))))
(define (sieve lst)
  (if (null? lst) '()
      (cons (car lst)
            (sieve (filter (lambda (x) (not (= 0 (modulo x (car lst)))))
                           (cdr lst))))))
(define (filter p lst)
  (cond ((null? lst) '())
        ((p (car lst)) (cons (car lst) (filter p (cdr lst))))
        (else (filter p (cdr lst)))))
(display (sieve (range 2 30))) (newline)

;; pascal's triangle row
(define (pascal-row n)
  (let loop ((i 0) (row '(1)))
    (if (= i n) row
        (loop (+ i 1)
              (cons 1
                    (let r ((row row))
                      (cond ((null? (cdr row)) row)
                            (else (cons (+ (car row) (cadr row))
                                        (r (cdr row)))))))))))
(display (pascal-row 5)) (newline)
