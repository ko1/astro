;; Sieve of Eratosthenes: count primes < n, using i32 memory as a bitmap byte array.
;; mem[i] = 0 → prime, 1 → composite.
(module
  (memory 16 64)   ;; 1 MB initial, 4 MB max

  (func $count_primes (export "count_primes") (param $n i32) (result i32)
    (local $i i32) (local $j i32) (local $count i32)

    ;; Mark composites: for i = 2..sqrt(n), if mem[i] == 0, mark j = i*i, i*i+i, ... < n
    (local.set $i (i32.const 2))
    (loop $outer
      (if (i32.lt_s (i32.mul (local.get $i) (local.get $i)) (local.get $n))
        (then
          (if (i32.eqz (i32.load8_u (local.get $i)))
            (then
              (local.set $j (i32.mul (local.get $i) (local.get $i)))
              (loop $inner
                (if (i32.lt_s (local.get $j) (local.get $n))
                  (then
                    (i32.store8 (local.get $j) (i32.const 1))
                    (local.set $j (i32.add (local.get $j) (local.get $i)))
                    (br $inner))))))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $outer))))

    ;; Count: i = 2..n where mem[i] == 0
    (local.set $i (i32.const 2))
    (loop $count_loop
      (if (i32.lt_s (local.get $i) (local.get $n))
        (then
          (if (i32.eqz (i32.load8_u (local.get $i)))
            (then (local.set $count (i32.add (local.get $count) (i32.const 1)))))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $count_loop))))

    (local.get $count)))
