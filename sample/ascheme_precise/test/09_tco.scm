;; proper tail calls — these should run in O(1) stack
(define (count-down n) (if (= n 0) 'done (count-down (- n 1))))
(display (count-down 1000000)) (newline)

;; named-let TCO
(display
  (let loop ((i 0) (acc 0))
    (if (= i 1000000) acc (loop (+ i 1) (+ acc 1)))))
(newline)

;; mutually recursive tail calls
(define (m-even? n) (if (= n 0) #t (m-odd?  (- n 1))))
(define (m-odd?  n) (if (= n 0) #f (m-even? (- n 1))))
(display (m-even? 100000)) (newline)

;; do loop
(display
  (do ((i 0 (+ i 1)) (s 0 (+ s i))) ((= i 1000) s)))
(newline)
