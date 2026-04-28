;; Conway's Game of Life on a 128x128 toroidal grid.
;;
;; Each generation visits 16K cells, performs 8 neighbour loads + a few
;; ALU ops, and writes one byte; with N generations the workload is
;; ~16K × 12 × N memory accesses + corresponding branches.  Toroidal
;; wraparound is computed with `& 127` so the inner loop stays
;; address-arithmetic only.
;;
;; The initial pattern is a deterministic LCG-seeded fill.  After
;; running N generations, the final 16K-byte grid is folded into a
;; multiplicative i32 hash.
;;
;;   life(1)    -> initial gen advanced once; checksum is stable
;;   life(200)  -> 200 generations; ~3M cell-updates

(module
  (memory 1)   ;; 64 KB; A=0..16K, B=16K..32K

  (global $W i32 (i32.const 128))
  (global $H i32 (i32.const 128))
  (global $N i32 (i32.const 16384))   ;; W*H
  (global $A_BASE i32 (i32.const 0))
  (global $B_BASE i32 (i32.const 16384))

  ;; LCG-seeded random fill of buffer A: cell = bit15 of the LCG state.
  (func $init
    (local $i i32) (local $rng i32) (local $n i32)
    (local.set $rng (i32.const 12345))
    (local.set $n (global.get $N))
    (block $end (loop $L
      (br_if $end (i32.ge_s (local.get $i) (local.get $n)))
      (local.set $rng
        (i32.add (i32.mul (local.get $rng) (i32.const 1103515245))
                 (i32.const 12345)))
      (i32.store8 (i32.add (global.get $A_BASE) (local.get $i))
        (i32.and (i32.shr_u (local.get $rng) (i32.const 16)) (i32.const 1)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $L))))

  ;; Compute one generation: B = step(A), then A := B.
  (func $step
    (local $x i32) (local $y i32) (local $cnt i32)
    (local $cell i32) (local $next i32)
    (local $xm i32) (local $xp i32) (local $ym i32) (local $yp i32)
    (local $row i32) (local $rowm i32) (local $rowp i32)
    (local $i i32)

    (block $y_end (loop $y_loop
      (br_if $y_end (i32.ge_s (local.get $y) (global.get $H)))
      (local.set $ym (i32.and (i32.sub (local.get $y) (i32.const 1)) (i32.const 127)))
      (local.set $yp (i32.and (i32.add (local.get $y) (i32.const 1)) (i32.const 127)))
      (local.set $row  (i32.shl (local.get $y)  (i32.const 7)))
      (local.set $rowm (i32.shl (local.get $ym) (i32.const 7)))
      (local.set $rowp (i32.shl (local.get $yp) (i32.const 7)))
      (local.set $x (i32.const 0))
      (block $x_end (loop $x_loop
        (br_if $x_end (i32.ge_s (local.get $x) (global.get $W)))
        (local.set $xm (i32.and (i32.sub (local.get $x) (i32.const 1)) (i32.const 127)))
        (local.set $xp (i32.and (i32.add (local.get $x) (i32.const 1)) (i32.const 127)))

        (local.set $cnt
          (i32.add
            (i32.add
              (i32.add
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $rowm) (local.get $xm))))
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $rowm) (local.get $x)))))
              (i32.add
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $rowm) (local.get $xp))))
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $row)  (local.get $xm))))))
            (i32.add
              (i32.add
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $row)  (local.get $xp))))
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $rowp) (local.get $xm)))))
              (i32.add
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $rowp) (local.get $x))))
                (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $rowp) (local.get $xp))))))))

        (local.set $cell
          (i32.load8_u (i32.add (global.get $A_BASE) (i32.add (local.get $row) (local.get $x)))))

        ;; Conway: next = (cnt==3) | (cell & cnt==2)
        (local.set $next
          (i32.or
            (i32.eq (local.get $cnt) (i32.const 3))
            (i32.and (local.get $cell) (i32.eq (local.get $cnt) (i32.const 2)))))

        (i32.store8 (i32.add (global.get $B_BASE) (i32.add (local.get $row) (local.get $x)))
                    (local.get $next))

        (local.set $x (i32.add (local.get $x) (i32.const 1)))
        (br $x_loop)))
      (local.set $y (i32.add (local.get $y) (i32.const 1)))
      (br $y_loop)))

    ;; Copy B back into A in 4-byte chunks.
    (block $cp_end (loop $cp_loop
      (br_if $cp_end (i32.ge_s (local.get $i) (global.get $N)))
      (i32.store (i32.add (global.get $A_BASE) (local.get $i))
                 (i32.load (i32.add (global.get $B_BASE) (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 4)))
      (br $cp_loop))))

  (func $life (export "life") (param $gens i32) (result i32)
    (local $i i32) (local $hash i32)
    (call $init)
    (block $g_end (loop $g_loop
      (br_if $g_end (i32.ge_s (local.get $i) (local.get $gens)))
      (call $step)
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $g_loop)))

    (local.set $i (i32.const 0))
    (block $h_end (loop $h_loop
      (br_if $h_end (i32.ge_s (local.get $i) (global.get $N)))
      (local.set $hash
        (i32.add (i32.mul (local.get $hash) (i32.const 31))
                 (i32.load8_u (i32.add (global.get $A_BASE) (local.get $i)))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $h_loop)))
    (local.get $hash)))
