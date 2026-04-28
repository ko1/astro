;; Brainfuck interpreter — runs an embedded BF program N times,
;; folding the output bytes into a multiplicative i32 hash.
;;
;; Not a microbenchmark: the inner loop is a 7-way opcode cascade
;; (`+`, `-`, `>`, `<`, `.`, `[`, `]`), each iteration touches the
;; tape and hops through a precomputed jump table.  A single
;; execution of the embedded 73-char program runs ~280 BF dispatches;
;; with iters=200000 the interpreter executes ~56M dispatches.
;;
;;   bf_run(1)        -> 1 execution; checksum 1873919810
;;   bf_run(200000)   -> 200000 executions; same arithmetic on hash

(module
  (memory 4)  ;; 256 KB

  ;; Layout:
  ;;   [    0 ..  4096) — BF program text, NUL-terminated
  ;;   [ 4096 .. 16384) — jump table (i32 per program byte)
  ;;   [16384 .. 32768) — BF tape (16 KB, used 1 byte per cell)

  (global $JUMP_BASE i32 (i32.const 4096))
  (global $TAPE_BASE i32 (i32.const 16384))
  (global $TAPE_SIZE i32 (i32.const 16384))

  ;; Hello-world-ish BF program: 73 chars, 8 output bytes per run.
  (data (i32.const 0)
    "++++++++[->+++++++++>++++++++++>+++<<<-]>.+++.+++++++..+++.>++.<<.>>++++.\00")

  (func $prog_len (result i32)
    (local $i i32)
    (block $done
      (loop $L
        (br_if $done (i32.eqz (i32.load8_u (local.get $i))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $L)))
    (local.get $i))

  ;; Walk the program once, recording matching `[`/`]` positions in the
  ;; jump table.  jt[ip(`[`)] = ip(`]`), jt[ip(`]`)] = ip(`[`).
  (func $precompute_jumps
    (local $i i32) (local $j i32) (local $depth i32)
    (local $n i32) (local $c i32)
    (local.set $n (call $prog_len))
    (block $done_outer (loop $L_outer
      (br_if $done_outer (i32.ge_s (local.get $i) (local.get $n)))
      (local.set $c (i32.load8_u (local.get $i)))
      (if (i32.eq (local.get $c) (i32.const 91))   ;; '['
        (then
          (local.set $depth (i32.const 1))
          (local.set $j (i32.add (local.get $i) (i32.const 1)))
          (block $found (loop $L_find
            (br_if $found (i32.ge_s (local.get $j) (local.get $n)))
            (local.set $c (i32.load8_u (local.get $j)))
            (if (i32.eq (local.get $c) (i32.const 91))
              (then (local.set $depth (i32.add (local.get $depth) (i32.const 1)))))
            (if (i32.eq (local.get $c) (i32.const 93))   ;; ']'
              (then
                (local.set $depth (i32.sub (local.get $depth) (i32.const 1)))
                (br_if $found (i32.eqz (local.get $depth)))))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $L_find)))
          (i32.store
            (i32.add (global.get $JUMP_BASE) (i32.shl (local.get $i) (i32.const 2)))
            (local.get $j))
          (i32.store
            (i32.add (global.get $JUMP_BASE) (i32.shl (local.get $j) (i32.const 2)))
            (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $L_outer))))

  ;; One full execution of the BF program; returns updated hash.
  (func $bf_once (param $hash i32) (result i32)
    (local $ip i32) (local $dp i32) (local $c i32)
    (local $n i32) (local $jt i32) (local $end i32)

    ;; Zero the tape (4 bytes per store).
    (local.set $dp (global.get $TAPE_BASE))
    (local.set $end (i32.add (local.get $dp) (global.get $TAPE_SIZE)))
    (block $zd (loop $zl
      (br_if $zd (i32.ge_s (local.get $dp) (local.get $end)))
      (i32.store (local.get $dp) (i32.const 0))
      (local.set $dp (i32.add (local.get $dp) (i32.const 4)))
      (br $zl)))

    (local.set $dp (global.get $TAPE_BASE))
    (local.set $jt (global.get $JUMP_BASE))
    (local.set $n  (call $prog_len))

    (block $exit (loop $L
      (br_if $exit (i32.ge_s (local.get $ip) (local.get $n)))
      (local.set $c (i32.load8_u (local.get $ip)))

      (block $disp
        (if (i32.eq (local.get $c) (i32.const 43))   ;; '+'
          (then
            (i32.store8 (local.get $dp)
              (i32.add (i32.load8_u (local.get $dp)) (i32.const 1)))
            (br $disp)))
        (if (i32.eq (local.get $c) (i32.const 45))   ;; '-'
          (then
            (i32.store8 (local.get $dp)
              (i32.sub (i32.load8_u (local.get $dp)) (i32.const 1)))
            (br $disp)))
        (if (i32.eq (local.get $c) (i32.const 62))   ;; '>'
          (then
            (local.set $dp (i32.add (local.get $dp) (i32.const 1)))
            (br $disp)))
        (if (i32.eq (local.get $c) (i32.const 60))   ;; '<'
          (then
            (local.set $dp (i32.sub (local.get $dp) (i32.const 1)))
            (br $disp)))
        (if (i32.eq (local.get $c) (i32.const 46))   ;; '.'
          (then
            (local.set $hash
              (i32.add (i32.mul (local.get $hash) (i32.const 31))
                       (i32.load8_u (local.get $dp))))
            (br $disp)))
        (if (i32.eq (local.get $c) (i32.const 91))   ;; '['
          (then
            (if (i32.eqz (i32.load8_u (local.get $dp)))
              (then
                (local.set $ip
                  (i32.load
                    (i32.add (local.get $jt) (i32.shl (local.get $ip) (i32.const 2)))))))
            (br $disp)))
        (if (i32.eq (local.get $c) (i32.const 93))   ;; ']'
          (then
            (if (i32.load8_u (local.get $dp))
              (then
                (local.set $ip
                  (i32.load
                    (i32.add (local.get $jt) (i32.shl (local.get $ip) (i32.const 2)))))))
            (br $disp))))

      (local.set $ip (i32.add (local.get $ip) (i32.const 1)))
      (br $L)))
    (local.get $hash))

  (func $bf_run (export "bf_run") (param $iters i32) (result i32)
    (local $i i32) (local $hash i32)
    (call $precompute_jumps)
    (block $done (loop $L
      (br_if $done (i32.ge_s (local.get $i) (local.get $iters)))
      (local.set $hash (call $bf_once (local.get $hash)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br $L)))
    (local.get $hash)))
