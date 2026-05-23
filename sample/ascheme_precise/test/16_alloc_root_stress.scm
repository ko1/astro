;; precise rooting gap detector.  Each top-level form exercises a pattern
;; that requires alloc-spanning VALUE temporaries to be properly rooted:
;;
;;   1. cons of heap-typed args (strings / pairs).  node_cons_op evals
;;      `a` and `b` as C locals; the `b` eval may allocate and move `a`.
;;   2. arith binops with bignum operands.  node_arith_add evals `a`
;;      then `b`; bignum eval allocates; without sp[]-rooting `a` would
;;      be stale by the time scm_make_int / scm_normalize_int runs.
;;   3. arithmetic compare with bignum operands (same hazard, cmp2 path).
;;   4. vector ops with bignum index / value.
;;   5. user-defined call with heap arg (call_K's `fn` C local across
;;      arg eval).
;;   6. call/cc holding a bignum result across capture/invoke.
;;
;; Designed to crash before the @child / sp-rooting refactor under
;; `make GC=copy_scramble`+BARUBY_GC_STRESS=1, and pass after.

;; 1. cons of heap-typed args — both sides allocate
(define (build-strs n)
  (let loop ((i 0) (acc '()))
    (if (< i n)
        (loop (+ i 1) (cons (number->string i) acc))
        acc)))
(display (length (build-strs 200))) (newline)

;; 2. arithmetic with bignum operand crossing alloc
(define (test-arith-bignum n)
  (let loop ((i 0) (acc 1))
    (if (< i n)
        (loop (+ i 1) (* acc 1103515245))
        (modulo acc 1000))))
(display (test-arith-bignum 50)) (newline)

;; 3. compare with bignum operand
(display (< (* 123456 789012) (* 234567 890123))) (newline)
(display (= (* 999999999999 1000000000001)
            (- (* 1000000000000 1000000000000) 1))) (newline)

;; 4. vector ops with bignum index/value
(define (test-vec-bignum)
  (let ((v (make-vector 4 0)))
    (vector-set! v 0 (* 12345 67890))
    (vector-set! v 1 (cons (* 99999 99999) "hello"))
    (vector-ref v 0)))
(display (test-vec-bignum)) (newline)

;; 5. call_K with heap arg — fn evaluated as a gref (cheap), arg
;;    constructs a heap list, then user-defined identity returns it.
(define (id x) x)
(display (id (cons 1 (cons 2 (cons 3 '()))))) (newline)

;; 6. call/cc holding a bignum result
(display (call/cc (lambda (k) (k (* 1000000 1000000))))) (newline)

;; 7. nested cons with heap-typed inner exprs (multiple alloc-crossing
;;    operands inside one cons call)
(display
  (cons (cons "a" "b") (cons (cons "c" "d") '()))) (newline)

;; 8. arith of bignum + small fixnum (lhs heap, rhs fix); add2 calls
;;    promotion routines that themselves allocate.
(display (+ (* 1000000000000 1000000000000) 1)) (newline)
(display (- 1 (* 1000000000000 1000000000000))) (newline)
