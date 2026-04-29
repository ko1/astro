;; delay / force (R5RS §6.4) + basic file I/O
(define p (delay (* 6 7)))
(display (force p)) (newline)         ; 42
(display (promise? p)) (newline)      ; #t
(display (promise? 5)) (newline)      ; #f

;; memoized
(define n 0)
(define q (delay (begin (set! n (+ n 1)) n)))
(force q) (force q) (force q)
(display n) (newline)                  ; 1 — thunk runs once

;; recursive use
(define (integers-from k)
  (cons k (delay (integers-from (+ k 1)))))
(define s (integers-from 1))
(display (car s)) (newline)            ; 1
(display (car (force (cdr s)))) (newline)            ; 2
(display (car (force (cdr (force (cdr s)))))) (newline)  ; 3

;; force on a non-promise is identity (R5RS-compatible extension)
(display (force 42)) (newline)         ; 42

;; write-to-string via temp file
(define tmp "/tmp/ascheme-test-port.txt")
(with-output-to-file tmp
  (lambda ()
    (display "hello ")
    (write 'world)
    (newline)))

(define p (open-input-file tmp))
(display (input-port? p)) (newline)    ; #t
(display (output-port? p)) (newline)   ; #f
(let loop ((ch (read-char p)) (acc '()))
  (if (eof-object? ch)
      (begin (display (reverse acc)) (newline))   ; list of chars
      (loop (read-char p) (cons ch acc))))
(close-input-port p)
