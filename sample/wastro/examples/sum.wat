;; Iterative sum 1..n using block + loop + br_if.
;; Demonstrates block / loop / br_if / local.set / iterative computation.
;;
;;   (block $exit
;;     (loop $body
;;       i >= n  →  br $exit
;;       sum += i
;;       i += 1
;;       br $body))
;;   sum
(module
  (func $sum (export "sum") (param $n i32) (result i32)
    (local $i i32)
    (local $s i32)
    (block $exit
      (loop $body
        (br_if $exit (i32.eq (local.get $i) (local.get $n)))
        (local.set $s (i32.add (local.get $s) (local.get $i)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $body)))
    (local.get $s)))
