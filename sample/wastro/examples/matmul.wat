;; Dense i32 matrix multiply C = A * B, square N×N.
;;
;; A is at memory[0..4*N*N), B is at memory[4*N*N .. 8*N*N), C at the
;; next N*N slots.  Init: A[i][j] = i+j, B[i][j] = i-j (mod 2^32).
;;
;; Returns sum of C as a checksum so we have a stable integer to
;; compare across runtimes.
;;
;;   matmul(64)  ≈ medium
;;   matmul(128) ≈ ~2M multiply-adds
;;   matmul(256) ≈ ~16M multiply-adds

(module
  (memory 16)   ;; 1 MB, enough for 3 × 256² × 4-byte matrices

  (func $idx (param $i i32) (param $j i32) (param $N i32) (result i32)
    (i32.mul
      (i32.add
        (i32.mul (local.get $i) (local.get $N))
        (local.get $j))
      (i32.const 4)))

  (func $base_a (result i32) (i32.const 0))
  (func $base_b (param $N i32) (result i32)
    (i32.mul (i32.mul (local.get $N) (local.get $N)) (i32.const 4)))
  (func $base_c (param $N i32) (result i32)
    (i32.mul
      (i32.mul (i32.mul (local.get $N) (local.get $N)) (i32.const 4))
      (i32.const 2)))

  (func $matmul (export "matmul") (param $N i32) (result i32)
    (local $i i32) (local $j i32) (local $k i32)
    (local $sum i32)
    (local $checksum i32)

    ;; Initialize A and B.
    (block $init_done
      (loop $init_i
        (br_if $init_done (i32.ge_s (local.get $i) (local.get $N)))
        (local.set $j (i32.const 0))
        (block $init_j_done
          (loop $init_j
            (br_if $init_j_done (i32.ge_s (local.get $j) (local.get $N)))
            ;; A[i][j] = i + j
            (i32.store
              (i32.add (call $base_a) (call $idx (local.get $i) (local.get $j) (local.get $N)))
              (i32.add (local.get $i) (local.get $j)))
            ;; B[i][j] = i - j
            (i32.store
              (i32.add (call $base_b (local.get $N))
                       (call $idx (local.get $i) (local.get $j) (local.get $N)))
              (i32.sub (local.get $i) (local.get $j)))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $init_j)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $init_i)))

    ;; C = A * B.
    (local.set $i (i32.const 0))
    (block $mul_done
      (loop $mul_i
        (br_if $mul_done (i32.ge_s (local.get $i) (local.get $N)))
        (local.set $j (i32.const 0))
        (block $mul_j_done
          (loop $mul_j
            (br_if $mul_j_done (i32.ge_s (local.get $j) (local.get $N)))
            (local.set $sum (i32.const 0))
            (local.set $k (i32.const 0))
            (block $mul_k_done
              (loop $mul_k
                (br_if $mul_k_done (i32.ge_s (local.get $k) (local.get $N)))
                (local.set $sum
                  (i32.add (local.get $sum)
                    (i32.mul
                      (i32.load
                        (i32.add (call $base_a)
                                 (call $idx (local.get $i) (local.get $k) (local.get $N))))
                      (i32.load
                        (i32.add (call $base_b (local.get $N))
                                 (call $idx (local.get $k) (local.get $j) (local.get $N)))))))
                (local.set $k (i32.add (local.get $k) (i32.const 1)))
                (br $mul_k)))
            (i32.store
              (i32.add (call $base_c (local.get $N))
                       (call $idx (local.get $i) (local.get $j) (local.get $N)))
              (local.get $sum))
            (local.set $checksum (i32.add (local.get $checksum) (local.get $sum)))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $mul_j)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $mul_i)))
    (local.get $checksum)))
