;; trunc_sat smoke test.
;;   sat_pos: f64 trunc_sat for huge positive (would trap with plain trunc)
;;   sat_neg: f64 trunc_sat for huge negative
;;   sat_nan: f64 trunc_sat for NaN (returns 0)
(module
  (func $sat_pos (export "sat_pos") (result i32)
    (i32.trunc_sat_f64_s (f64.const 1e20)))     ;; → INT32_MAX
  (func $sat_neg (export "sat_neg") (result i32)
    (i32.trunc_sat_f64_s (f64.const -1e20)))    ;; → INT32_MIN
  (func $sat_nan (export "sat_nan") (result i32)
    (i32.trunc_sat_f64_s (f64.div (f64.const 0) (f64.const 0))))  ;; NaN → 0
  (func $sat_normal (export "sat_normal") (result i32)
    (i32.trunc_sat_f64_s (f64.const 3.7))))     ;; → 3
