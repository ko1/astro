;; "Hello, world!\n" via env.print_bytes (host import) + (data ...).
;;
;; The (data ...) writes 14 bytes at offset 0 in linear memory at
;; instantiation; main calls env.print_bytes(0, 14) to write them
;; to stdout.
(module
  (import "env" "print_bytes" (func $print (param i32 i32)))
  (memory 1)
  (data (i32.const 0) "Hello, world!\n")

  (func $main (export "main")
    (call $print (i32.const 0) (i32.const 14))))
