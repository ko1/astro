(define (sum n acc) (if (= n 0) acc (sum (- n 1) (+ acc n))))
(display (sum 10000000 0)) (newline)
