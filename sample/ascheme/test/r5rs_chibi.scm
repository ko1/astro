;; Adapted from chibi-scheme tests/r5rs-tests.scm.  See
;; test/r5rs_adapt.rb for the transformation we apply.
(define *tests-run* 0)
(define *tests-passed* 0)
(define (test-proc . args)
  ;; (test-proc expected actual)  or  (test-proc name expected actual)
  (set! *tests-run* (+ *tests-run* 1))
  (let ((expected (if (= (length args) 2) (car args) (cadr args)))
        (actual   (if (= (length args) 2) (cadr args) (caddr args))))
    (cond
     ((equal? actual expected)
      (set! *tests-passed* (+ *tests-passed* 1)))
     (else
      (display "FAIL #") (display *tests-run*)
      (display " expected=") (write expected)
      (display " actual=") (write actual) (newline)))))
(define (test-assert-proc x) (test-proc #t x))
(define (test-begin . n) #f)
(define (test-end)
  (display *tests-passed*) (display " / ") (display *tests-run*)
  (display " r5rs tests passed") (newline))
(define (flush-output) #f)               ;; noop — chibi's test driver flushes stdout
(define call-with-output-string
  (lambda (proc)
    (let ((s "<dummy>")) (proc 'dummy-port) s))) ;; just stub for the test driver
(define *tests-run* 0)

(define *tests-passed* 0)

(define (test-begin . name)
  #f)

(define (test-end)
  (write *tests-passed*)
  (display " out of ")
  (write *tests-run*)
  (display " passed (")
  (write (* (/ *tests-passed* *tests-run*) 100))
  (display "%)")
  (newline))

(test-begin "r5rs")

(test-proc 8 ((lambda (x) (+ x x)) 4))

(test-proc '(3 4 5 6) ((lambda x x) 3 4 5 6))

(test-proc '(5 6) ((lambda (x y . z) z) 3 4 5 6))

(test-proc 'yes (if (> 3 2) 'yes 'no))

(test-proc 'no (if (> 2 3) 'yes 'no))

(test-proc 1 (if (> 3 2) (- 3 2) (+ 3 2)))

(test-proc 'greater (cond ((> 3 2) 'greater) ((< 3 2) 'less)))

(test-proc 'equal (cond ((> 3 3) 'greater) ((< 3 3) 'less) (else 'equal)))

(test-proc 'composite (case (* 2 3) ((2 3 5 7) 'prime) ((1 4 6 8 9) 'composite)))

(test-proc 'consonant
    (case (car '(c d))
      ((a e i o u) 'vowel)
      ((w y) 'semivowel)
      (else 'consonant)))

(test-proc #t (and (= 2 2) (> 2 1)))

(test-proc #f (and (= 2 2) (< 2 1)))

(test-proc '(f g) (and 1 2 'c '(f g)))

(test-proc #t (and))

(test-proc #t (or (= 2 2) (> 2 1)))

(test-proc #t (or (= 2 2) (< 2 1)))

(test-proc '(b c) (or (memq 'b '(a b c)) (/ 3 0)))

(test-proc 6 (let ((x 2) (y 3)) (* x y)))

(test-proc 35 (let ((x 2) (y 3)) (let ((x 7) (z (+ x y))) (* z x))))

(test-proc 70 (let ((x 2) (y 3)) (let* ((x 7) (z (+ x y))) (* z x))))

(test-proc -2 (let ()
           (define x 2)
           (define f (lambda () (- x)))
           (f)))

(define let*-def 1)

(let* () (define let*-def 2) #f)

(test-proc 1 let*-def)

(test-proc '#(0 1 2 3 4)
 (do ((vec (make-vector 5))
      (i 0 (+ i 1)))
     ((= i 5) vec)
   (vector-set! vec i i)))

(test-proc 25
    (let ((x '(1 3 5 7 9)))
      (do ((x x (cdr x))
           (sum 0 (+ sum (car x))))
          ((null? x)
           sum))))

(test-proc '((6 1 3) (-5 -2))
    (let loop ((numbers '(3 -2 1 6 -5)) (nonneg '()) (neg '()))
      (cond
       ((null? numbers)
        (list nonneg neg))
       ((>= (car numbers) 0)
        (loop (cdr numbers) (cons (car numbers) nonneg) neg))
       ((< (car numbers) 0)
        (loop (cdr numbers) nonneg (cons (car numbers) neg))))))

(test-proc '(list 3 4) `(list ,(+ 1 2) 4))

(test-proc '(list a 'a) (let ((name 'a)) `(list ,name ',name)))

(test-proc '(a 3 4 5 6 b)
    `(a ,(+ 1 2) ,@(map abs '(4 -5 6)) b))

(test-proc '(10 5 4 16 9 8)
    `(10 5 ,(expt 2 2) ,@(map (lambda (n) (expt n 2)) '(4 3)) 8))

(test-proc '(a `(b ,(+ 1 2) ,(foo 4 d) e) f)
    `(a `(b ,(+ 1 2) ,(foo ,(+ 1 3) d) e) f))

(test-proc '(a `(b ,x ,'y d) e)
    (let ((name1 'x)
          (name2 'y))
      `(a `(b ,,name1 ,',name2 d) e)))

(test-proc '(list 3 4)
 (quasiquote (list (unquote (+ 1 2)) 4)))

(test-proc #t (eqv? 'a 'a))

(test-proc #f (eqv? 'a 'b))

(test-proc #t (eqv? '() '()))

(test-proc #f (eqv? (cons 1 2) (cons 1 2)))

(test-proc #f (eqv? (lambda () 1) (lambda () 2)))

(test-proc #t (let ((p (lambda (x) x))) (eqv? p p)))

(test-proc #t (eq? 'a 'a))

(test-proc #f (eq? (list 'a) (list 'a)))

(test-proc #t (eq? '() '()))

(test-proc #t (eq? car car))

(test-proc #t (let ((x '(a))) (eq? x x)))

(test-proc #t (let ((p (lambda (x) x))) (eq? p p)))

(test-proc #t (equal? 'a 'a))

(test-proc #t (equal? '(a) '(a)))

(test-proc #t (equal? '(a (b) c) '(a (b) c)))

(test-proc #t (equal? "abc" "abc"))

(test-proc #f (equal? "abc" "abcd"))

(test-proc #f (equal? "a" "b"))

(test-proc #t (equal? 2 2))

(test-proc #t (equal? (make-vector 5 'a) (make-vector 5 'a)))

(test-proc 4 (max 3 4))

(test-proc 7 (+ 3 4))

(test-proc 3 (+ 3))

(test-proc 0 (+))

(test-proc 4 (* 4))

(test-proc 1 (*))

(test-proc -1 (- 3 4))

(test-proc -6 (- 3 4 5))

(test-proc -3 (- 3))

(test-proc -1.0 (- 3.0 4))

(test-proc 7 (abs -7))

(test-proc 1 (modulo 13 4))

(test-proc 1 (remainder 13 4))

(test-proc 3 (modulo -13 4))

(test-proc -1 (remainder -13 4))

(test-proc -3 (modulo 13 -4))

(test-proc 1 (remainder 13 -4))

(test-proc -1 (modulo -13 -4))

(test-proc -1 (remainder -13 -4))

(test-proc 4 (gcd 32 -36))

(test-proc 288 (lcm 32 -36))

(test-proc 100 (string->number "100"))

(test-proc 256 (string->number "100" 16))

(test-proc 127 (string->number "177" 8))

(test-proc 5 (string->number "101" 2))

(test-proc 100.0 (string->number "1e2"))

(test-proc "100" (number->string 100))

(test-proc "100" (number->string 256 16))

(test-proc "ff" (number->string 255 16))

(test-proc "177" (number->string 127 8))

(test-proc "101" (number->string 5 2))

(test-proc #f (not 3))

(test-proc #f (not (list 3)))

(test-proc #f (not '()))

(test-proc #f (not (list)))

(test-proc #f (not '()))

(test-proc #f (boolean? 0))

(test-proc #f (boolean? '()))

(test-proc #t (pair? '(a . b)))

(test-proc #t (pair? '(a b c)))

(test-proc '(a) (cons 'a '()))

(test-proc '((a) b c d) (cons '(a) '(b c d)))

(test-proc '("a" b c) (cons "a" '(b c)))

(test-proc '(a . 3) (cons 'a 3))

(test-proc '((a b) . c) (cons '(a b) 'c))

(test-proc 'a (car '(a b c)))

(test-proc '(a) (car '((a) b c d)))

(test-proc 1 (car '(1 . 2)))

(test-proc '(b c d) (cdr '((a) b c d)))

(test-proc 2 (cdr '(1 . 2)))

(test-proc #t (list? '(a b c)))

(test-proc #t (list? '()))

(test-proc #f (list? '(a . b)))

(test-proc #f
    (let ((x (list 'a)))
      (set-cdr! x x)
      (list? x)))

(test-proc '(a 7 c) (list 'a (+ 3 4) 'c))

(test-proc '() (list))

(test-proc 3 (length '(a b c)))

(test-proc 3 (length '(a (b) (c d e))))

(test-proc 0 (length '()))

(test-proc '(x y) (append '(x) '(y)))

(test-proc '(a b c d) (append '(a) '(b c d)))

(test-proc '(a (b) (c)) (append '(a (b)) '((c))))

(test-proc '(a b c . d) (append '(a b) '(c . d)))

(test-proc 'a (append '() 'a))

(test-proc '(c b a) (reverse '(a b c)))

(test-proc '((e (f)) d (b c) a) (reverse '(a (b c) d (e (f)))))

(test-proc 'c (list-ref '(a b c d) 2))

(test-proc '(a b c) (memq 'a '(a b c)))

(test-proc '(b c) (memq 'b '(a b c)))

(test-proc #f (memq 'a '(b c d)))

(test-proc #f (memq (list 'a) '(b (a) c)))

(test-proc '((a) c) (member (list 'a) '(b (a) c)))

(test-proc '(101 102) (memv 101 '(100 101 102)))

(test-proc #f (assq (list 'a) '(((a)) ((b)) ((c)))))

(test-proc '((a)) (assoc (list 'a) '(((a)) ((b)) ((c)))))

(test-proc '(5 7) (assv 5 '((2 3) (5 7) (11 13))))

(test-proc #t (symbol? 'foo))

(test-proc #t (symbol? (car '(a b))))

(test-proc #f (symbol? "bar"))

(test-proc #t (symbol? 'nil))

(test-proc #f (symbol? '()))

(test-proc "flying-fish" (symbol->string 'flying-fish))

(test-proc "Martin" (symbol->string 'Martin))

(test-proc "Malvina" (symbol->string (string->symbol "Malvina")))

(test-proc #t (string? "a"))

(test-proc #f (string? 'a))

(test-proc 0 (string-length ""))

(test-proc 3 (string-length "abc"))

(test-proc #\a (string-ref "abc" 0))

(test-proc #\c (string-ref "abc" 2))

(test-proc #t (string=? "a" (string #\a)))

(test-proc #f (string=? "a" (string #\b)))

(test-proc #t (string<? "a" "aa"))

(test-proc #f (string<? "aa" "a"))

(test-proc #f (string<? "a" "a"))

(test-proc #t (string<=? "a" "aa"))

(test-proc #t (string<=? "a" "a"))

(test-proc #t (string=? "a" (make-string 1 #\a)))

(test-proc #f (string=? "a" (make-string 1 #\b)))

(test-proc "" (substring "abc" 0 0))

(test-proc "a" (substring "abc" 0 1))

(test-proc "bc" (substring "abc" 1 3))

(test-proc "abc" (string-append "abc" ""))

(test-proc "abc" (string-append "" "abc"))

(test-proc "abc" (string-append "a" "bc"))

(test-proc '#(0 ("Sue" "Sue") "Anna")
 (let ((vec (vector 0 '(2 2 2 2) "Anna")))
   (vector-set! vec 1 '("Sue" "Sue"))
   vec))

(test-proc '(dah dah didah) (vector->list '#(dah dah didah)))

(test-proc '#(dididit dah) (list->vector '(dididit dah)))

(test-proc #t (procedure? car))

(test-proc #f (procedure? 'car))

(test-proc #t (procedure? (lambda (x) (* x x))))

(test-proc #f (procedure? '(lambda (x) (* x x))))

(test-proc #t (call-with-current-continuation procedure?))

(test-proc 7 (call-with-current-continuation (lambda (k) (+ 2 5))))

(test-proc 3 (call-with-current-continuation (lambda (k) (+ 2 5 (k 3)))))

(test-proc 7 (apply + (list 3 4)))

(test-proc '(b e h) (map cadr '((a b) (d e) (g h))))

(test-proc '(1 4 27 256 3125) (map (lambda (n) (expt n n)) '(1 2 3 4 5)))

(test-proc '(5 7 9) (map + '(1 2 3) '(4 5 6)))

(test-proc '#(0 1 4 9 16)
    (let ((v (make-vector 5)))
      (for-each
       (lambda (i) (vector-set! v i (* i i)))
       '(0 1 2 3 4))
      v))

(test-proc 3 (force (delay (+ 1 2))))

(test-proc '(3 3) (let ((p (delay (+ 1 2)))) (list (force p) (force p))))

(test-proc 'ok (let ((else 1)) (cond (else 'ok) (#t 'bad))))

(test-proc 'ok (let ((=> 1)) (cond (#t => 'ok))))

(test-proc '(,foo) (let ((unquote 1)) `(,foo)))

(test-proc '(,@foo) (let ((unquote-splicing 1)) `(,@foo)))

(test-proc '(2 1)
    ((lambda () (let ((x 1)) (let ((y x)) (set! x 2) (list x y))))))

(test-proc '(2 2)
    ((lambda () (let ((x 1)) (set! x 2) (let ((y x)) (list x y))))))

(test-proc '(1 2)
    ((lambda () (let ((x 1)) (let ((y x)) (set! y 2) (list x y))))))

(test-proc '(2 3)
    ((lambda () (let ((x 1)) (let ((y x)) (set! x 2) (set! y 3) (list x y))))))

(test-proc '(a b c)
    (let* ((path '())
           (add (lambda (s) (set! path (cons s path)))))
      (dynamic-wind (lambda () (add 'a)) (lambda () (add 'b)) (lambda () (add 'c)))
      (reverse path)))

(test-proc '(connect talk1 disconnect connect talk2 disconnect)
    (let ((path '())
          (c #f))
      (let ((add (lambda (s)
                   (set! path (cons s path)))))
        (dynamic-wind
            (lambda () (add 'connect))
            (lambda ()
              (add (call-with-current-continuation
                    (lambda (c0)
                      (set! c c0)
                      'talk1))))
            (lambda () (add 'disconnect)))
        (if (< (length path) 4)
            (c 'talk2)
            (reverse path)))))

(test-end)

