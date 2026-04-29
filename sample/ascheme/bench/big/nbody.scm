;; N-body simulation (5 bodies) — gravitational attraction.  Standard
;; Computer Language Benchmarks Game entry; exercises flonum arithmetic
;; and vector access in the inner loop.
;;
;; Each body is a 7-vector: x y z vx vy vz mass.

(define +pi+         3.141592653589793)
(define +days+       365.24)
(define +solar-mass+ (* 4.0 +pi+ +pi+))

(define (body x y z vx vy vz mass)
  (vector x y z vx vy vz mass))

(define bodies
  (list
    ;; sun
    (body 0.0 0.0 0.0 0.0 0.0 0.0 +solar-mass+)
    ;; jupiter
    (body 4.84143144246472090e0
          -1.16032004402742839e0
          -1.03622044471123109e-1
          (* 1.66007664274403694e-3 +days+)
          (* 7.69901118419740425e-3 +days+)
          (* -6.90460016972063023e-5 +days+)
          (* 9.54791938424326609e-4 +solar-mass+))
    ;; saturn
    (body 8.34336671824457987e0
          4.12479856412430479e0
          -4.03523417114321381e-1
          (* -2.76742510726862411e-3 +days+)
          (* 4.99852801234917238e-3 +days+)
          (* 2.30417297573763929e-5 +days+)
          (* 2.85885980666130812e-4 +solar-mass+))
    ;; uranus
    (body 1.28943695621391310e1
          -1.51111514016986312e1
          -2.23307578892655734e-1
          (* 2.96460137564761618e-3 +days+)
          (* 2.37847173959480950e-3 +days+)
          (* -2.96589568540237556e-5 +days+)
          (* 4.36624404335156298e-5 +solar-mass+))
    ;; neptune
    (body 1.53796971148509165e1
          -2.59193146099879641e1
          1.79258772950371181e-1
          (* 2.68067772490389322e-3 +days+)
          (* 1.62824170038242295e-3 +days+)
          (* -9.51592254519715870e-5 +days+)
          (* 5.15138902046611451e-5 +solar-mass+))))

(define (offset-momentum bs)
  (let loop ((bs bs) (px 0.0) (py 0.0) (pz 0.0))
    (cond ((null? bs)
           (let ((sun (car bodies)))
             (vector-set! sun 3 (/ (- px) +solar-mass+))
             (vector-set! sun 4 (/ (- py) +solar-mass+))
             (vector-set! sun 5 (/ (- pz) +solar-mass+))))
          (else
           (let ((b (car bs)))
             (loop (cdr bs)
                   (+ px (* (vector-ref b 3) (vector-ref b 6)))
                   (+ py (* (vector-ref b 4) (vector-ref b 6)))
                   (+ pz (* (vector-ref b 5) (vector-ref b 6)))))))))

(define (energy bs)
  (let loop ((bs bs) (e 0.0))
    (if (null? bs) e
        (let* ((b (car bs))
               (vx (vector-ref b 3))
               (vy (vector-ref b 4))
               (vz (vector-ref b 5))
               (m  (vector-ref b 6))
               (e0 (+ e (* 0.5 m (+ (* vx vx) (* vy vy) (* vz vz))))))
          (let pair ((rest (cdr bs)) (e e0))
            (if (null? rest) (loop (cdr bs) e)
                (let* ((b2 (car rest))
                       (dx (- (vector-ref b 0) (vector-ref b2 0)))
                       (dy (- (vector-ref b 1) (vector-ref b2 1)))
                       (dz (- (vector-ref b 2) (vector-ref b2 2)))
                       (d  (sqrt (+ (* dx dx) (* dy dy) (* dz dz)))))
                  (pair (cdr rest)
                        (- e (/ (* m (vector-ref b2 6)) d))))))))))

(define (advance bs dt)
  ;; pairwise force update
  (let outer ((bs bs))
    (if (not (null? bs))
        (let ((b (car bs)))
          (let inner ((rest (cdr bs)))
            (if (not (null? rest))
                (let* ((b2 (car rest))
                       (dx (- (vector-ref b 0) (vector-ref b2 0)))
                       (dy (- (vector-ref b 1) (vector-ref b2 1)))
                       (dz (- (vector-ref b 2) (vector-ref b2 2)))
                       (d2 (+ (* dx dx) (* dy dy) (* dz dz)))
                       (d  (sqrt d2))
                       (mag (/ dt (* d2 d)))
                       (mb  (vector-ref b 6))
                       (mb2 (vector-ref b2 6)))
                  (vector-set! b  3 (- (vector-ref b 3)  (* dx mb2 mag)))
                  (vector-set! b  4 (- (vector-ref b 4)  (* dy mb2 mag)))
                  (vector-set! b  5 (- (vector-ref b 5)  (* dz mb2 mag)))
                  (vector-set! b2 3 (+ (vector-ref b2 3) (* dx mb  mag)))
                  (vector-set! b2 4 (+ (vector-ref b2 4) (* dy mb  mag)))
                  (vector-set! b2 5 (+ (vector-ref b2 5) (* dz mb  mag)))
                  (inner (cdr rest)))))
          (outer (cdr bs)))))
  ;; position update
  (for-each (lambda (b)
              (vector-set! b 0 (+ (vector-ref b 0) (* dt (vector-ref b 3))))
              (vector-set! b 1 (+ (vector-ref b 1) (* dt (vector-ref b 4))))
              (vector-set! b 2 (+ (vector-ref b 2) (* dt (vector-ref b 5)))))
            bs))

(offset-momentum bodies)
(let loop ((i 0))
  (if (< i 50000) (begin (advance bodies 0.01) (loop (+ i 1)))))
(display (energy bodies))
(newline)
