;; N-queens by exhaustive backtracking — exercises lists + recursion.
;; (nqueens 12) → 14200 solutions.
(define (safe? row placed col)
  (cond ((null? placed) #t)
        ((= row (car placed)) #f)
        ((= (abs (- row (car placed))) col) #f)
        (else (safe? row (cdr placed) (+ col 1)))))

(define (queens n placed remaining)
  (if (= remaining 0)
      1
      (let loop ((row 1) (count 0))
        (if (> row n)
            count
            (loop (+ row 1)
                  (if (safe? row placed 1)
                      (+ count (queens n (cons row placed) (- remaining 1)))
                      count))))))

(define (nqueens n) (queens n '() n))

(display (nqueens 12))
(newline)
