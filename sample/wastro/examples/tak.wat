(module
  (func $tak (export "tak") (param $x i32) (param $y i32) (param $z i32) (result i32)
    (if (result i32) (i32.lt_s (local.get $y) (local.get $x))
      (then
        (call $tak
          (call $tak (i32.sub (local.get $x) (i32.const 1)) (local.get $y) (local.get $z))
          (call $tak (i32.sub (local.get $y) (i32.const 1)) (local.get $z) (local.get $x))
          (call $tak (i32.sub (local.get $z) (i32.const 1)) (local.get $x) (local.get $y))))
      (else (local.get $z)))))
