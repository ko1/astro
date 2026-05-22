;; higher-order functions
(display (map (lambda (x) (* x x)) '(1 2 3 4 5))) (newline)
(display (map + '(1 2 3) '(10 20 30))) (newline)
(display (apply + '(1 2 3 4 5))) (newline)
(display (apply max '(3 1 4 1 5 9 2 6))) (newline)
(display (apply + 1 2 '(3 4 5))) (newline)

;; for-each
(define first #t)
(for-each (lambda (x)
            (if first (set! first #f) (display " "))
            (display x))
          '(a b c))
(newline)

;; recursion
(define (length-rec lst)
  (if (null? lst) 0 (+ 1 (length-rec (cdr lst)))))
(display (length-rec '(a b c d e))) (newline)

;; mutual recursion
(define (even-r? n) (if (= n 0) #t (odd-r? (- n 1))))
(define (odd-r?  n) (if (= n 0) #f (even-r? (- n 1))))
(display (even-r? 1000)) (newline)
(display (odd-r? 999)) (newline)

;; Y-combinator-style
(define (Y f)
  ((lambda (x) (f (lambda (n) ((x x) n))))
   (lambda (x) (f (lambda (n) ((x x) n))))))
(define fact-y
  (Y (lambda (rec)
       (lambda (n) (if (= n 0) 1 (* n (rec (- n 1))))))))
(display (fact-y 10)) (newline)

;; sorting (insertion)
(define (insert x sorted)
  (if (null? sorted)
      (list x)
      (if (<= x (car sorted))
          (cons x sorted)
          (cons (car sorted) (insert x (cdr sorted))))))
(define (sort lst)
  (if (null? lst) '() (insert (car lst) (sort (cdr lst)))))
(display (sort '(5 2 8 1 9 3 7 4 6))) (newline)
