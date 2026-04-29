;; list construction + traversal — cons + linear scan
(define (build n)
  (let loop ((i 0) (acc '()))
    (if (= i n) acc (loop (+ i 1) (cons i acc)))))
(define (sum lst)
  (let loop ((l lst) (s 0))
    (if (null? l) s (loop (cdr l) (+ s (car l))))))
(display (sum (build 3000000)))
(newline)
