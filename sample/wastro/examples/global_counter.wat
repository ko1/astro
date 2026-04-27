;; Counter via mutable global + global.get / global.set.
(module
  (global $count (mut i32) (i32.const 0))

  (func $tick (export "tick") (param $n i32) (result i32)
    (local $i i32)
    (loop $body
      (if (i32.lt_s (local.get $i) (local.get $n))
        (then
          (global.set $count (i32.add (global.get $count) (i32.const 1)))
          (local.set $i (i32.add (local.get $i) (i32.const 1)))
          (br $body))))
    (global.get $count)))
