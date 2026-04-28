;; Register-machine bytecode VM running an embedded Fibonacci program.
;;
;; The VM has 16 i32 registers and 12 opcodes; each instruction is a
;; fixed 8 bytes (op:1 / a:1 / b:1 / c:1 / imm:4 LE).  The embedded
;; bytecode iteratively computes fib(0) .. fib(n-1) and folds each
;; value into a multiplicative i32 hash.
;;
;; This is meta-interpretation: ASTro evaluates a wasm AST that itself
;; evaluates a bytecode program, so the inner dispatch loop is a chain
;; of `i32.eq` against opcode bytes plus reg-file loads/stores —
;; representative of how a guest VM in wasm gets implemented.
;;
;;   vm_bench(20)       -> fib(0..19) hash
;;   vm_bench(1000000)  -> ~8M bytecode dispatches

(module
  (memory 1)

  ;; Bytecode program — see comments for the high-level mnemonics.
  ;; Instruction layout: \op \a \b \c \imm0 \imm1 \imm2 \imm3
  (data (i32.const 0)
    "\00\00\00\00\00\00\00\00"   ;;   0  LOADI   r0, 0
    "\00\01\00\00\01\00\00\00"   ;;   8  LOADI   r1, 1
    "\00\02\00\00\00\00\00\00"   ;;  16  LOADI   r2, 0
    "\01\03\0a\00\00\00\00\00"   ;;  24  MOV     r3, r10        (r10 = N from runtime)
    "\06\04\02\03\00\00\00\00"   ;;  32  LT      r4, r2, r3
    "\09\04\00\00\60\00\00\00"   ;;  40  BR_ZERO r4, 96         (-> HALT)
    "\0a\01\00\00\00\00\00\00"   ;;  48  HASH    r1
    "\02\05\00\01\00\00\00\00"   ;;  56  ADD     r5, r0, r1
    "\01\00\01\00\00\00\00\00"   ;;  64  MOV     r0, r1
    "\01\01\05\00\00\00\00\00"   ;;  72  MOV     r1, r5
    "\05\02\02\00\01\00\00\00"   ;;  80  ADDI    r2, r2, 1
    "\07\00\00\00\20\00\00\00"   ;;  88  BR      32             (loop back to LT)
    "\0b\00\00\00\00\00\00\00"   ;;  96  HALT
  )

  (global $PROG_LEN i32 (i32.const 104))
  (global $REGS i32 (i32.const 4096))   ;; 16 i32 registers

  (func $vm_run (param $arg i32) (param $hash i32) (result i32)
    (local $ip i32) (local $op i32) (local $a i32) (local $b i32) (local $c i32) (local $imm i32)
    (local $regs i32) (local $i i32)

    (local.set $regs (global.get $REGS))

    ;; clear regs (16 × 4 = 64 bytes)
    (block $rc_end (loop $rc_loop
      (br_if $rc_end (i32.ge_s (local.get $i) (i32.const 64)))
      (i32.store (i32.add (local.get $regs) (local.get $i)) (i32.const 0))
      (local.set $i (i32.add (local.get $i) (i32.const 4)))
      (br $rc_loop)))

    ;; reg[10] = arg  (offset 40 = 10 * 4)
    (i32.store (i32.add (local.get $regs) (i32.const 40)) (local.get $arg))

    (block $halt (loop $L
      (local.set $op  (i32.load8_u (local.get $ip)))
      (local.set $a   (i32.load8_u (i32.add (local.get $ip) (i32.const 1))))
      (local.set $b   (i32.load8_u (i32.add (local.get $ip) (i32.const 2))))
      (local.set $c   (i32.load8_u (i32.add (local.get $ip) (i32.const 3))))
      (local.set $imm (i32.load    (i32.add (local.get $ip) (i32.const 4))))

      (block $disp
        ;; LOADI: reg[a] = imm
        (if (i32.eq (local.get $op) (i32.const 0))
          (then
            (i32.store (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))
                       (local.get $imm))
            (br $disp)))
        ;; MOV: reg[a] = reg[b]
        (if (i32.eq (local.get $op) (i32.const 1))
          (then
            (i32.store (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))
                       (i32.load (i32.add (local.get $regs) (i32.shl (local.get $b) (i32.const 2)))))
            (br $disp)))
        ;; ADD: reg[a] = reg[b] + reg[c]
        (if (i32.eq (local.get $op) (i32.const 2))
          (then
            (i32.store (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))
                       (i32.add
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $b) (i32.const 2))))
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $c) (i32.const 2))))))
            (br $disp)))
        ;; SUB: reg[a] = reg[b] - reg[c]
        (if (i32.eq (local.get $op) (i32.const 3))
          (then
            (i32.store (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))
                       (i32.sub
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $b) (i32.const 2))))
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $c) (i32.const 2))))))
            (br $disp)))
        ;; MUL: reg[a] = reg[b] * reg[c]
        (if (i32.eq (local.get $op) (i32.const 4))
          (then
            (i32.store (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))
                       (i32.mul
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $b) (i32.const 2))))
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $c) (i32.const 2))))))
            (br $disp)))
        ;; ADDI: reg[a] = reg[b] + imm
        (if (i32.eq (local.get $op) (i32.const 5))
          (then
            (i32.store (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))
                       (i32.add
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $b) (i32.const 2))))
                         (local.get $imm)))
            (br $disp)))
        ;; LT: reg[a] = (reg[b] < reg[c]) ? 1 : 0
        (if (i32.eq (local.get $op) (i32.const 6))
          (then
            (i32.store (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))
                       (i32.lt_s
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $b) (i32.const 2))))
                         (i32.load (i32.add (local.get $regs) (i32.shl (local.get $c) (i32.const 2))))))
            (br $disp)))
        ;; BR: ip = imm
        (if (i32.eq (local.get $op) (i32.const 7))
          (then
            (local.set $ip (i32.sub (local.get $imm) (i32.const 8)))
            (br $disp)))
        ;; BR_IF: if reg[a] != 0 then ip = imm
        (if (i32.eq (local.get $op) (i32.const 8))
          (then
            (if (i32.load (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2))))
              (then (local.set $ip (i32.sub (local.get $imm) (i32.const 8)))))
            (br $disp)))
        ;; BR_ZERO: if reg[a] == 0 then ip = imm
        (if (i32.eq (local.get $op) (i32.const 9))
          (then
            (if (i32.eqz (i32.load (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2)))))
              (then (local.set $ip (i32.sub (local.get $imm) (i32.const 8)))))
            (br $disp)))
        ;; HASH: hash = hash*31 + reg[a]
        (if (i32.eq (local.get $op) (i32.const 10))
          (then
            (local.set $hash
              (i32.add (i32.mul (local.get $hash) (i32.const 31))
                       (i32.load (i32.add (local.get $regs) (i32.shl (local.get $a) (i32.const 2))))))
            (br $disp)))
        ;; HALT
        (if (i32.eq (local.get $op) (i32.const 11))
          (then (br $halt))))

      (local.set $ip (i32.add (local.get $ip) (i32.const 8)))
      (br $L)))
    (local.get $hash))

  (func $vm_bench (export "vm_bench") (param $n i32) (result i32)
    (call $vm_run (local.get $n) (i32.const 0))))
