(import (scheme base) (scheme write))
(define (abs x) (if (< x 0) (- 0 x) x))
(define (nqueens n)
  (define (safe? row col placed)
    (let loop ((p placed) (d 1))
      (if (null? p) #t
          (let ((q (car p)))
            (if (or (= q col) (= (abs (- q col)) d)) #f
                (loop (cdr p) (+ d 1)))))))
  (define (place row placed)
    (if (= row n) 1
        (let loop ((col 0) (cnt 0))
          (if (= col n) cnt
              (if (safe? row col placed)
                  (loop (+ col 1) (+ cnt (place (+ row 1) (cons col placed))))
                  (loop (+ col 1) cnt))))))
  (place 0 '()))
(display (nqueens 10)) (newline)
