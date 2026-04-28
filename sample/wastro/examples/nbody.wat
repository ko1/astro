;; N-body — minimal 2-body Earth-Sun orbit, leapfrog integrator.
;;
;; Tracks Earth's position (x, y) and velocity (vx, vy) around a fixed
;; sun at the origin, using G*M_sun = 1, dt = 0.001.  After `steps`
;; integration steps, returns (x*1e6 as i64) — a stable integer
;; fingerprint of the orbit position.
;;
;; This is purely f64-arithmetic with no allocations or memory ops:
;; an apples-to-apples FP-loop comparison against wasmtime.
;;
;;   nbody(1000)    -> small-orbit position
;;   nbody(100000)  -> longer-running benchmark

(module
  (func $nbody (export "nbody") (param $steps i32) (result i64)
    (local $x f64)  (local $y f64)
    (local $vx f64) (local $vy f64)
    (local $r f64)  (local $r3 f64)
    (local $i i32)
    (local $dt f64)
    ;; Initial circular orbit at radius 1, with v_perp = 1.
    (local.set $x  (f64.const 1.0))
    (local.set $y  (f64.const 0.0))
    (local.set $vx (f64.const 0.0))
    (local.set $vy (f64.const 1.0))
    (local.set $dt (f64.const 0.001))

    (block $done
      (loop $L
        (br_if $done (i32.ge_s (local.get $i) (local.get $steps)))
        ;; r = sqrt(x*x + y*y); r3 = r * r * r
        (local.set $r
          (f64.sqrt
            (f64.add
              (f64.mul (local.get $x) (local.get $x))
              (f64.mul (local.get $y) (local.get $y)))))
        (local.set $r3
          (f64.mul (local.get $r) (f64.mul (local.get $r) (local.get $r))))
        ;; ax = -x / r3, ay = -y / r3
        ;; vx += ax*dt, vy += ay*dt
        (local.set $vx
          (f64.sub (local.get $vx)
            (f64.div (f64.mul (local.get $x) (local.get $dt)) (local.get $r3))))
        (local.set $vy
          (f64.sub (local.get $vy)
            (f64.div (f64.mul (local.get $y) (local.get $dt)) (local.get $r3))))
        ;; x += vx*dt, y += vy*dt
        (local.set $x (f64.add (local.get $x) (f64.mul (local.get $vx) (local.get $dt))))
        (local.set $y (f64.add (local.get $y) (f64.mul (local.get $vy) (local.get $dt))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $L)))
    ;; return x * 1e6 as i64
    (i64.trunc_f64_s
      (f64.mul (local.get $x) (f64.const 1000000.0)))))
