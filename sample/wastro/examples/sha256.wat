;; SHA-256 over a small in-memory buffer.
;;
;; Compresses an N-block message of 512 bits each (filled with byte
;; pattern (i + k) mod 256, then padded to one full final block).
;; Returns the first 4 bytes of the digest as i32 — a stable
;; integer fingerprint that depends on N.
;;
;; Heavy in i32 bit ops (rotr / xor / and / shr) — the kind of code
;; that typical wasm crypto modules run.
;;
;;   sha256_bench(1)   -> hash of 1 block of 64 bytes
;;   sha256_bench(100) -> hash of ~6.4 KB
;;   sha256_bench(10000) -> hash of ~640 KB

(module
  (memory 32)   ;; 2 MB

  ;; Round constants K — 64 entries, stored at byte offset 0.
  (data (i32.const 0)
    "\98\2f\8a\42\91\44\37\71\cf\fb\c0\b5\a5\db\b5\e9"
    "\5b\c2\56\39\f1\11\f1\59\a4\82\3f\92\d5\5e\1c\ab"
    "\98\aa\07\d8\01\5b\83\12\be\85\31\24\c3\7d\0c\55"
    "\74\5d\be\72\fe\b1\de\80\a7\06\dc\9b\74\f1\9b\c1"
    "\c1\69\9b\e4\86\47\be\ef\c6\9d\c1\0f\cc\a1\0c\24"
    "\6f\2c\e9\2d\aa\84\74\4a\dc\a9\b0\5c\da\88\f9\76"
    "\52\51\3e\98\6d\c6\31\a8\c8\27\03\b0\c7\7f\59\bf"
    "\f3\0b\e0\c6\47\91\a7\d5\51\63\ca\06\67\29\29\14"
    "\85\0a\b7\27\38\21\1b\2e\fc\6d\2c\4d\13\0d\38\53"
    "\54\73\0a\65\bb\0a\6a\76\2e\c9\c2\81\85\2c\72\92"
    "\a1\e8\bf\a2\4b\66\1a\a8\70\8b\4b\c2\a3\51\6c\c7"
    "\19\e8\92\d1\24\06\99\d6\85\35\0e\f4\70\a0\6a\10"
    "\16\c1\a4\19\08\6c\37\1e\4c\77\48\27\b5\bc\b0\34"
    "\b3\0c\1c\39\4a\aa\d8\4e\4f\ca\9c\5b\f3\6f\2e\68"
    "\ee\82\8f\74\6f\63\a5\78\14\78\c8\84\08\02\c7\8c"
    "\fa\ff\be\90\eb\6c\50\a4\f7\a3\f9\be\f2\78\71\c6")

  ;; Initial hash values H — 8 entries at byte offset 256.
  (data (i32.const 256)
    "\67\e6\09\6a\85\ae\67\bb\72\f3\6e\3c\3a\f5\4f\a5"
    "\7f\52\0e\51\8c\68\05\9b\ab\d9\83\1f\19\cb\e6\7b"
    "\f6\70\6e\0c\d1\9e\89\68\a4\09\08\41\79\e0\ed\97"
    "\1f\83\d9\ab\5b\e0\cd\19\c4\6e\db\f5\54\fa\7c\18")

  ;; rotr32 — i32 rotate-right by n.
  (func $rotr (param $x i32) (param $n i32) (result i32)
    (i32.rotr (local.get $x) (local.get $n)))

  ;; State H lives at memory[256..256+32).
  ;; Message block W lives at memory[512..512+256).

  ;; Reset state to initial H from the data segment.
  (func $reset
    (local $i i32)
    (block $done
      (loop $L
        (br_if $done (i32.ge_s (local.get $i) (i32.const 8)))
        (i32.store
          (i32.add (i32.const 256) (i32.mul (local.get $i) (i32.const 4)))
          (i32.load
            (i32.add (i32.const 256) (i32.mul (local.get $i) (i32.const 4)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $L))))

  ;; Big-endian u32 load from byte offset.
  (func $load_be (param $off i32) (result i32)
    (i32.or
      (i32.or
        (i32.shl (i32.load8_u (local.get $off)) (i32.const 24))
        (i32.shl (i32.load8_u (i32.add (local.get $off) (i32.const 1))) (i32.const 16)))
      (i32.or
        (i32.shl (i32.load8_u (i32.add (local.get $off) (i32.const 2))) (i32.const 8))
        (i32.load8_u (i32.add (local.get $off) (i32.const 3))))))

  ;; Compress one 64-byte block at the given byte offset into H.
  (func $compress (param $blk i32)
    (local $i i32)
    (local $w0 i32) (local $s0 i32) (local $s1 i32)
    (local $a i32) (local $b i32) (local $c i32) (local $d i32)
    (local $e i32) (local $f i32) (local $g i32) (local $h i32)
    (local $t1 i32) (local $t2 i32)
    (local $ch i32) (local $maj i32)
    (local $S0 i32) (local $S1 i32)

    ;; W[0..16] = block bytes (big-endian).
    (block $copy_done
      (loop $copy_L
        (br_if $copy_done (i32.ge_s (local.get $i) (i32.const 16)))
        (i32.store
          (i32.add (i32.const 512) (i32.mul (local.get $i) (i32.const 4)))
          (call $load_be
            (i32.add (local.get $blk) (i32.mul (local.get $i) (i32.const 4)))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $copy_L)))

    ;; W[16..64] = sigma1(W[i-2]) + W[i-7] + sigma0(W[i-15]) + W[i-16]
    (local.set $i (i32.const 16))
    (block $expand_done
      (loop $expand_L
        (br_if $expand_done (i32.ge_s (local.get $i) (i32.const 64)))
        (local.set $w0
          (i32.load (i32.add (i32.const 512) (i32.mul (i32.sub (local.get $i) (i32.const 15)) (i32.const 4)))))
        (local.set $s0
          (i32.xor
            (i32.xor (call $rotr (local.get $w0) (i32.const 7))
                     (call $rotr (local.get $w0) (i32.const 18)))
            (i32.shr_u (local.get $w0) (i32.const 3))))
        (local.set $w0
          (i32.load (i32.add (i32.const 512) (i32.mul (i32.sub (local.get $i) (i32.const 2)) (i32.const 4)))))
        (local.set $s1
          (i32.xor
            (i32.xor (call $rotr (local.get $w0) (i32.const 17))
                     (call $rotr (local.get $w0) (i32.const 19)))
            (i32.shr_u (local.get $w0) (i32.const 10))))
        (i32.store
          (i32.add (i32.const 512) (i32.mul (local.get $i) (i32.const 4)))
          (i32.add
            (i32.add
              (i32.load (i32.add (i32.const 512) (i32.mul (i32.sub (local.get $i) (i32.const 16)) (i32.const 4))))
              (local.get $s0))
            (i32.add
              (i32.load (i32.add (i32.const 512) (i32.mul (i32.sub (local.get $i) (i32.const 7)) (i32.const 4))))
              (local.get $s1))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $expand_L)))

    (local.set $a (i32.load (i32.const 256)))
    (local.set $b (i32.load (i32.const 260)))
    (local.set $c (i32.load (i32.const 264)))
    (local.set $d (i32.load (i32.const 268)))
    (local.set $e (i32.load (i32.const 272)))
    (local.set $f (i32.load (i32.const 276)))
    (local.set $g (i32.load (i32.const 280)))
    (local.set $h (i32.load (i32.const 284)))

    (local.set $i (i32.const 0))
    (block $round_done
      (loop $round_L
        (br_if $round_done (i32.ge_s (local.get $i) (i32.const 64)))
        (local.set $S1
          (i32.xor
            (i32.xor (call $rotr (local.get $e) (i32.const 6))
                     (call $rotr (local.get $e) (i32.const 11)))
            (call $rotr (local.get $e) (i32.const 25))))
        (local.set $ch
          (i32.xor
            (i32.and (local.get $e) (local.get $f))
            (i32.and (i32.xor (local.get $e) (i32.const -1)) (local.get $g))))
        (local.set $t1
          (i32.add
            (i32.add (i32.add (local.get $h) (local.get $S1))
                     (i32.add (local.get $ch)
                              (i32.load (i32.add (i32.const 0) (i32.mul (local.get $i) (i32.const 4))))))
            (i32.load (i32.add (i32.const 512) (i32.mul (local.get $i) (i32.const 4))))))
        (local.set $S0
          (i32.xor
            (i32.xor (call $rotr (local.get $a) (i32.const 2))
                     (call $rotr (local.get $a) (i32.const 13)))
            (call $rotr (local.get $a) (i32.const 22))))
        (local.set $maj
          (i32.xor
            (i32.xor
              (i32.and (local.get $a) (local.get $b))
              (i32.and (local.get $a) (local.get $c)))
            (i32.and (local.get $b) (local.get $c))))
        (local.set $t2 (i32.add (local.get $S0) (local.get $maj)))

        (local.set $h (local.get $g))
        (local.set $g (local.get $f))
        (local.set $f (local.get $e))
        (local.set $e (i32.add (local.get $d) (local.get $t1)))
        (local.set $d (local.get $c))
        (local.set $c (local.get $b))
        (local.set $b (local.get $a))
        (local.set $a (i32.add (local.get $t1) (local.get $t2)))

        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $round_L)))

    (i32.store (i32.const 256) (i32.add (i32.load (i32.const 256)) (local.get $a)))
    (i32.store (i32.const 260) (i32.add (i32.load (i32.const 260)) (local.get $b)))
    (i32.store (i32.const 264) (i32.add (i32.load (i32.const 264)) (local.get $c)))
    (i32.store (i32.const 268) (i32.add (i32.load (i32.const 268)) (local.get $d)))
    (i32.store (i32.const 272) (i32.add (i32.load (i32.const 272)) (local.get $e)))
    (i32.store (i32.const 276) (i32.add (i32.load (i32.const 276)) (local.get $f)))
    (i32.store (i32.const 280) (i32.add (i32.load (i32.const 280)) (local.get $g)))
    (i32.store (i32.const 284) (i32.add (i32.load (i32.const 284)) (local.get $h))))

  ;; sha256_bench(N) — fill an N-block buffer at memory[1024..),
  ;; compress all of them, return the first u32 of the digest (BE).
  (func $sha256_bench (export "sha256_bench") (param $N i32) (result i32)
    (local $i i32) (local $j i32)
    (call $reset)
    ;; Fill memory[1024 + i*64 .. + 64) with byte (i + j) % 256.
    (block $fill_done
      (loop $fill_i
        (br_if $fill_done (i32.ge_s (local.get $i) (local.get $N)))
        (local.set $j (i32.const 0))
        (block $fill_j_done
          (loop $fill_j_L
            (br_if $fill_j_done (i32.ge_s (local.get $j) (i32.const 64)))
            (i32.store8
              (i32.add (i32.const 1024) (i32.add (i32.mul (local.get $i) (i32.const 64)) (local.get $j)))
              (i32.add (local.get $i) (local.get $j)))
            (local.set $j (i32.add (local.get $j) (i32.const 1)))
            (br $fill_j_L)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $fill_i)))
    ;; Compress each block.
    (local.set $i (i32.const 0))
    (block $hash_done
      (loop $hash_L
        (br_if $hash_done (i32.ge_s (local.get $i) (local.get $N)))
        (call $compress (i32.add (i32.const 1024) (i32.mul (local.get $i) (i32.const 64))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $hash_L)))
    (i32.load (i32.const 256))))
