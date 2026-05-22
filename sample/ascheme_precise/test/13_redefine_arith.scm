;; R5RS allows redefining standard procedures.  Verify that ascheme's
;; specialized arith / comparison nodes correctly fall back to the user's
;; redefinition rather than silently using the original primitives.

;; (1) Redefine + and verify the side effect is observed.
(define call-log '())
(define old+ +)
(set! + (lambda (a b)
          (set! call-log (cons '+called call-log))
          (old+ a b)))
(display (+ 3 4)) (newline)              ; expect: 7
(display (+ 10 20)) (newline)             ; expect: 30
(display (length call-log)) (newline)     ; expect: 2

;; (2) Restore + and verify subsequent calls resume the fast path.
(set! + old+)
(set! call-log '())
(display (+ 1 2)) (newline)               ; expect: 3
(display (length call-log)) (newline)     ; expect: 0

;; (3) Comparison ops are also specialized — verify <, =, etc. honour
;; redefinition.
(define old< <)
(set! < (lambda (a b) (old< b a)))        ; reverse the comparison
(display (< 1 2)) (newline)               ; expect: #f (reversed)
(display (< 5 1)) (newline)               ; expect: #t (reversed)
(set! < old<)
(display (< 1 2)) (newline)               ; expect: #t

;; (4) Mid-execution redefinition: a hot loop sees the old + first, then
;; the new + after the set!.
(define total 0)
(define (loop n)
  (if (= n 0)
      total
      (begin (set! total (+ total 1))
             (loop (- n 1)))))

(loop 5)
(display total) (newline)                 ; expect: 5

(set! + (lambda (a b) (- a b)))           ; + now subtracts
(loop 3)                                  ; total = 5, then "+1" three times
(display total) (newline)                 ; expect: 5 + (-1) * 3 = 2
(set! + old+)

;; (5) Specialization with non-fixnum args still respects redefinition.
(set! * (lambda (a b) 'replaced))
(display (* 2 3)) (newline)               ; expect: replaced
(set! * (lambda args (apply (lambda (a b) (* 0 0)) args)))   ; deliberate self-reference
(set! * old+)                             ; restore through old+ (now subtraction won't matter, we replace)
;; The above is just to exercise — final cleanup:
(define (real* a b) ((lambda (acc i) (if (= i 0) acc (* a (- b 1)))) 0 b))
;; Actually just put * back to original via a fresh lookup style:
;; (re-define via define which updates the same slot)
(define * (lambda (a b)
            (let loop ((acc 0) (i 0))
              (if (= i b) acc (loop (old+ acc a) (old+ i 1))))))
(display (* 4 5)) (newline)               ; expect: 20
