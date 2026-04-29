;; sum 1..N via named-let — accumulator + TCO
(display
  (let loop ((i 0) (s 0))
    (if (= i 25000000) s (loop (+ i 1) (+ s i)))))
(newline)
