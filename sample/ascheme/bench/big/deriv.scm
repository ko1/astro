;; Symbolic differentiation, applied repeatedly.  Exercises list ops,
;; symbol comparisons (eq?), recursive tree walking — the classic
;; symbolic-computation workload.

(define (deriv expr var)
  (cond
    ((number? expr) 0)
    ((symbol? expr) (if (eq? expr var) 1 0))
    ((pair? expr)
     (let ((op (car expr)))
       (cond
         ((eq? op '+)
          (list '+ (deriv (cadr expr) var) (deriv (caddr expr) var)))
         ((eq? op '-)
          (list '- (deriv (cadr expr) var) (deriv (caddr expr) var)))
         ((eq? op '*)
          (let ((u (cadr expr)) (v (caddr expr)))
            (list '+
                  (list '* (deriv u var) v)
                  (list '* u (deriv v var)))))
         ((eq? op '/)
          (let ((u (cadr expr)) (v (caddr expr)))
            (list '/
                  (list '- (list '* (deriv u var) v)
                            (list '* u (deriv v var)))
                  (list '* v v))))
         ((eq? op 'expt)
          (let ((u (cadr expr)) (n (caddr expr)))
            (list '* (list '* n (list 'expt u (- n 1)))
                     (deriv u var))))
         (else (error "unknown op")))))
    (else 0)))

(define expr
  '(+ (* (expt x 5) (+ x 3))
      (* (- x 2) (/ (* x x) (+ x 1)))))

;; Differentiate the *same* expression a fixed number of times.  Each
;; `deriv` call allocates a fresh tree of cons cells, so this is mostly
;; an allocator + GC + tree-walking workload.
(let loop ((i 0) (last 0))
  (if (= i 200000)
      (begin (display last) (newline))
      (loop (+ i 1) (length (deriv expr 'x)))))
