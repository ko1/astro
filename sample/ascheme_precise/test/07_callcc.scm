;; call/cc — escape continuations only
(display (call/cc (lambda (k) 42))) (newline)
(display (call/cc (lambda (k) (+ 1 2 (k 99) 100)))) (newline)

;; early exit from a fold
(define (find pred lst)
  (call/cc
    (lambda (return)
      (for-each (lambda (x) (if (pred x) (return x) #f)) lst)
      #f)))
(display (find odd? '(2 4 6 7 8))) (newline)
(display (find (lambda (x) (> x 10)) '(1 2 3))) (newline)

;; multiplicative product with early termination on 0
(define (product lst)
  (call/cc
    (lambda (return)
      (let loop ((lst lst) (acc 1))
        (cond ((null? lst) acc)
              ((= 0 (car lst)) (return 0))
              (else (loop (cdr lst) (* acc (car lst)))))))))
(display (product '(1 2 3 4 5))) (newline)
(display (product '(1 2 0 4 5))) (newline)
