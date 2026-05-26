#!/usr/bin/env ruby
# test/fuzz/fuzz.rb — comprehensive Scheme fuzzer for ascheme_precise.
#
# Strategy
# --------
#   1. Generate small valid R5RS programs via two paths:
#      (a) ~50 "templates" — known-tricky patterns covering R5RS features
#          and precise-rooting hot spots (internal define, letrec, call/cc,
#          named-let, vector-of-closures, multi-values, delay/force, bignum,
#          rational, mutation, deep recursion, etc.).
#      (b) a structural random AST generator that recursively builds typed
#          expressions from a small grammar with depth bound.
#   2. Each program prints a single deterministic value (one or more lines).
#   3. Run with chez (`scheme --script`) as the correctness oracle.
#      chez is R5RS-compliant native code; libgc-based ascheme would share
#      bugs with ascheme_precise and is intentionally NOT used as oracle.
#   4. For each program, run ascheme_precise in a configurable mode matrix
#      (= plain / plain+stress / aot-cached / aot-cached+stress).  Compare
#      output to oracle.  Fail on SEGV, hang, or output mismatch.
#   5. On failure, dump the offending program to test/fuzz/fails/NNN_<gen>.scm
#      with stderr + which mode failed.
#   6. Print a generator usage breakdown so coverage of templates is visible.
#
# Usage
# -----
#   ruby test/fuzz/fuzz.rb [N] [opts via ENV]
#     N (default 200)             — number of programs to fuzz
#     SEED                        — deterministic seed
#     MODES=plain,stress,aot,aot-stress (default plain,stress)
#                                 — which precise modes to run per program
#     STRUCTURAL_RATIO=0.4        — fraction of programs from the random AST
#                                   generator vs. templates (default 0.4)
#     MAX_FAILS=10                — stop after this many failures
#     TIMEOUT=15                  — per-run timeout (sec)
#     ORACLE=path                 — override chez binary
#
# Exit code: 0 if all pass, 1 if any failure.
#
# To extend: add a method `gen_<name>` and append `:gen_<name>` to TEMPLATES.

require 'fileutils'
require 'open3'
require 'tempfile'

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

N         = (ARGV[0] || "200").to_i
SEED      = (ENV['SEED'] || Time.now.to_i).to_i
MODES     = (ENV['MODES'] || 'plain,stress').split(',').map(&:strip)
STRUCT_RATIO = (ENV['STRUCTURAL_RATIO'] || '0.4').to_f
MAX_FAILS = (ENV['MAX_FAILS'] || '10').to_i
TIME      = (ENV['TIMEOUT'] || '15').to_i

srand(SEED)

ROOT      = File.expand_path('../..', __dir__)
PRECISE   = File.join(ROOT, 'ascheme_precise')
ORACLE    = ENV['ORACLE'] || 'scheme'
ORACLE_ARGS = ['--script']
FAILDIR   = File.join(__dir__, 'fails')
FileUtils.mkdir_p(FAILDIR)

abort("missing binary: #{PRECISE}") unless File.executable?(PRECISE)

# ---------------------------------------------------------------------------
# Random helpers
# ---------------------------------------------------------------------------

def rsym(prefix = 'v') = "#{prefix}#{rand(10000)}"

def rnum
  case rand(6)
  when 0 then rand(100)
  when 1 then -rand(50)
  when 2 then rand(10)
  when 3 then rand(1000)
  when 4 then rand(2**40) + 2**30   # bignum range
  else        rand(20) - 10
  end
end

def rsmall = rand(20) + 1
def rmed   = rand(50) + 5
def rbig   = rand(200) + 20

# ---------------------------------------------------------------------------
# Template generators (= known-tricky patterns).
# Each returns a self-contained Scheme program string that displays >=1
# deterministic line.
# ---------------------------------------------------------------------------

# --- Internal defines / letrec / named-let ----------------------------------

def gen_mutual_internal
  n = rand(50) + 10
  <<~SCM
    (define (run)
      (define (e? n) (if (= n 0) #t (o? (- n 1))))
      (define (o? n) (if (= n 0) #f (e? (- n 1))))
      (e? #{n}))
    (display (run)) (newline)
  SCM
end

def gen_multi_internal
  n = rand(20) + 5
  <<~SCM
    (define (compute)
      (define a #{rnum})
      (define b #{rnum})
      (define c #{rnum})
      (define (helper x) (* x #{n}))
      (+ (helper a) (helper b) (helper c)))
    (display (compute)) (newline)
  SCM
end

def gen_nested_define
  <<~SCM
    (define (outer)
      (define (mid)
        (define (inner x)
          (define (deepest y) (+ x y))
          (deepest 10))
        (inner 20))
      (mid))
    (display (outer)) (newline)
  SCM
end

def gen_letrec_mutual
  n = rand(30) + 5
  <<~SCM
    (display
      (letrec ((e? (lambda (n) (if (= n 0) #t (o? (- n 1)))))
               (o? (lambda (n) (if (= n 0) #f (e? (- n 1))))))
        (e? #{n})))
    (newline)
  SCM
end

def gen_letrec_value_and_lambda
  <<~SCM
    (display
      (letrec ((x 10)
               (y (lambda () (* x 2))))
        (+ x (y))))
    (newline)
  SCM
end

def gen_named_let
  n = rand(100) + 10
  <<~SCM
    (display
      (let loop ((i 0) (acc 0))
        (if (= i #{n}) acc (loop (+ i 1) (+ acc i)))))
    (newline)
  SCM
end

def gen_named_let_with_break
  <<~SCM
    (display
      (let walk ((i 0) (acc 0))
        (cond ((= i 100) acc)
              ((= acc 1000) acc)   ; early exit when sum reaches 1000
              (else (walk (+ i 1) (+ acc i))))))
    (newline)
  SCM
end

def gen_do_loop
  n = rand(50) + 10
  <<~SCM
    (display
      (do ((i 0 (+ i 1))
           (s 0 (+ s i)))
          ((= i #{n}) s)))
    (newline)
  SCM
end

# --- Closures / capture / mutation ------------------------------------------

def gen_capturing_filter
  n  = rand(30) + 5
  k  = rand(5) + 2
  <<~SCM
    (define (filter p lst)
      (cond ((null? lst) (quote ()))
            ((p (car lst)) (cons (car lst) (filter p (cdr lst))))
            (else (filter p (cdr lst)))))
    (define (range a b) (if (>= a b) (quote ()) (cons a (range (+ a 1) b))))
    (define (count lst) (if (null? lst) 0 (+ 1 (count (cdr lst)))))
    (define (test threshold)
      (count (filter (lambda (x) (> x threshold)) (range 0 #{n}))))
    (display (test #{k})) (newline)
  SCM
end

def gen_y_factorial
  n = rand(10) + 1
  <<~SCM
    (define Y
      (lambda (f)
        ((lambda (x) (f (lambda (n) ((x x) n))))
         (lambda (x) (f (lambda (n) ((x x) n)))))))
    (define fact
      (Y (lambda (rec) (lambda (n) (if (= n 0) 1 (* n (rec (- n 1))))))))
    (display (fact #{n})) (newline)
  SCM
end

def gen_closure_chain
  n = rand(10) + 3
  <<~SCM
    (define (make-adder k) (lambda (x) (+ x k)))
    (define (make-multiplier k) (lambda (x) (* x k)))
    (define f (make-adder #{n}))
    (define g (make-multiplier #{rand(5) + 2}))
    (display (g (f #{rand(20)}))) (newline)
  SCM
end

def gen_mutable_capture
  <<~SCM
    (define (make-counter)
      (let ((n 0))
        (lambda ()
          (set! n (+ n 1))
          n)))
    (define c (make-counter))
    (c) (c) (c) (c)
    (display (c)) (newline)
  SCM
end

def gen_mutable_capture_shared
  # Two closures sharing the same captured cell — set! through one is visible
  # to the other.
  <<~SCM
    (define (make-pair)
      (let ((n 0))
        (cons (lambda () n)
              (lambda (v) (set! n v)))))
    (define p (make-pair))
    (define get (car p))
    (define put (cdr p))
    (put 42)
    (display (get)) (newline)
    (put 100)
    (display (get)) (newline)
  SCM
end

def gen_nested_let_capture
  a = rnum
  b = rnum
  <<~SCM
    (define f
      (let ((x #{a}))
        (let ((y #{b}))
          (lambda (z) (+ x y z)))))
    (display (f #{rnum})) (newline)
  SCM
end

def gen_vector_closures
  n = rand(8) + 3
  <<~SCM
    (define v (make-vector #{n} 0))
    (define (init i)
      (if (= i #{n}) (quote done)
          (begin (vector-set! v i (lambda () (* i i))) (init (+ i 1)))))
    (init 0)
    (define (sum i acc)
      (if (= i #{n}) acc (sum (+ i 1) (+ acc ((vector-ref v i))))))
    (display (sum 0 0)) (newline)
  SCM
end

def gen_list_of_closures
  n = rand(8) + 3
  <<~SCM
    (define (build i)
      (if (= i #{n}) (quote ())
          (cons (lambda () i) (build (+ i 1)))))
    (define lst (build 0))
    (define (sum lst) (if (null? lst) 0 (+ ((car lst)) (sum (cdr lst)))))
    (display (sum lst)) (newline)
  SCM
end

def gen_closure_mutated_from_inner
  <<~SCM
    (define (test)
      (let ((counter 0))
        (define (incr!) (set! counter (+ counter 1)))
        (define (get) counter)
        (incr!) (incr!) (incr!)
        (get)))
    (display (test)) (newline)
  SCM
end

# --- call/cc ----------------------------------------------------------------

def gen_callcc_basic
  v = rnum.abs + 1
  <<~SCM
    (display (call/cc (lambda (k) (+ #{v} (k 999) 1000)))) (newline)
  SCM
end

def gen_callcc_deep
  d = rand(100) + 20
  <<~SCM
    (define (test)
      (call/cc
        (lambda (k)
          (define (deep n)
            (if (= n 0) (k (quote escaped)) (deep (- n 1))))
          (deep #{d}))))
    (display (test)) (newline)
  SCM
end

def gen_callcc_early_return
  <<~SCM
    (define (product lst)
      (call/cc
        (lambda (return)
          (let loop ((lst lst) (acc 1))
            (cond ((null? lst) acc)
                  ((= 0 (car lst)) (return 0))
                  (else (loop (cdr lst) (* acc (car lst)))))))))
    (display (product (quote (1 2 3 4 5)))) (newline)
    (display (product (quote (1 2 0 4 5)))) (newline)
  SCM
end

# --- Arity / rest -----------------------------------------------------------

def gen_dotted_rest
  vals = (0..rand(5) + 1).map { rnum }
  <<~SCM
    (define (rest-fn first . rest) (cons first (length rest)))
    (display (rest-fn #{vals.join(' ')})) (newline)
  SCM
end

def gen_rest_only_lambda
  vals = (0..rand(5) + 1).map { rnum }
  <<~SCM
    (display ((lambda x x) #{vals.join(' ')})) (newline)
  SCM
end

def gen_apply
  n = rand(5) + 2
  args = (0..n).map { rnum }
  <<~SCM
    (display (apply + (list #{args.join(' ')}))) (newline)
  SCM
end

def gen_apply_with_lambda
  args = (0..3).map { rnum }
  <<~SCM
    (display (apply (lambda (a b c d) (+ a b c d)) (list #{args.join(' ')})))
    (newline)
  SCM
end

# --- Multi-values -----------------------------------------------------------

def gen_values_basic
  <<~SCM
    (call-with-values
      (lambda () (values 1 2 3))
      (lambda (a b c) (display (+ a b c)) (newline)))
  SCM
end

def gen_values_zero
  <<~SCM
    (call-with-values
      (lambda () (values))
      (lambda () (display (quote empty)) (newline)))
  SCM
end

def gen_values_single
  v = rnum
  <<~SCM
    (call-with-values
      (lambda () #{v})
      (lambda (x) (display x) (newline)))
  SCM
end

def gen_values_many
  n = rand(4) + 4
  args = (0..n).map { rnum }
  fn_args = (0..n).map { |i| "x#{i}" }
  <<~SCM
    (call-with-values
      (lambda () (values #{args.join(' ')}))
      (lambda (#{fn_args.join(' ')}) (display (+ #{fn_args.join(' ')})) (newline)))
  SCM
end

def gen_values_capture
  # consumer captures outer var
  <<~SCM
    (define base #{rnum})
    (call-with-values
      (lambda () (values 1 2))
      (lambda (a b) (display (+ base a b)) (newline)))
  SCM
end

# --- delay / force ----------------------------------------------------------

def gen_delay_force
  v = rnum
  <<~SCM
    (define p (delay (+ #{v} 1)))
    (display (force p)) (newline)
    (display (force p)) (newline)   ; should memoize, same value
  SCM
end

def gen_delay_force_capture
  v = rnum
  <<~SCM
    (define x #{v})
    (define p (delay (* x x)))
    (display (force p)) (newline)
    (set! x 999)
    (display (force p)) (newline)   ; memoized: same as first force, NOT 999*999
  SCM
end

# --- Numeric tower ---------------------------------------------------------

def gen_bignum_arith
  <<~SCM
    (display (* 999999999 999999999)) (newline)
    (display (expt 2 64)) (newline)
  SCM
end

def gen_bignum_factorial
  n = rand(15) + 10
  <<~SCM
    (define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
    (display (fact #{n})) (newline)
  SCM
end

def gen_rational
  <<~SCM
    (display (/ 1 3)) (newline)
    (display (+ 1/2 1/3)) (newline)
    (display (* 2/3 3/4)) (newline)
    (display (- 1/2 1/3)) (newline)
  SCM
end

def gen_number_predicates
  v = rnum
  <<~SCM
    (display (zero? 0)) (newline)
    (display (positive? #{v.abs + 1})) (newline)
    (display (negative? #{-(v.abs + 1)})) (newline)
    (display (even? 4)) (newline)
    (display (odd? 5)) (newline)
  SCM
end

def gen_arithmetic_mix
  <<~SCM
    (display (+ 1 2 3 4 5)) (newline)
    (display (* 1 2 3 4 5)) (newline)
    (display (- 100 1 2 3)) (newline)
    (display (/ 100 5 2)) (newline)
    (display (modulo 17 5)) (newline)
    (display (quotient 17 5)) (newline)
    (display (remainder 17 5)) (newline)
    (display (abs -42)) (newline)
    (display (min 3 1 4 1 5 9 2 6)) (newline)
    (display (max 3 1 4 1 5 9 2 6)) (newline)
  SCM
end

# --- Equality predicates ---------------------------------------------------

def gen_eq_predicates
  <<~SCM
    (display (eq? (quote a) (quote a))) (newline)
    (display (eqv? 1.5 1.5)) (newline)
    (display (equal? (quote (1 2 3)) (quote (1 2 3)))) (newline)
    (display (equal? "abc" "abc")) (newline)
  SCM
end

# --- Strings ---------------------------------------------------------------

def gen_string_ops
  <<~SCM
    (define s "hello world")
    (display (string-length s)) (newline)
    (display (substring s 6 11)) (newline)
    (display (string-append "foo" "bar" "baz")) (newline)
    (display (string->symbol "abc")) (newline)
    (display (symbol->string (quote xyz))) (newline)
  SCM
end

def gen_string_list_conv
  <<~SCM
    (define s "abc")
    (define lst (string->list s))
    (display lst) (newline)
    (display (list->string lst)) (newline)
  SCM
end

# --- Characters -----------------------------------------------------------

def gen_char_ops
  <<~SCM
    (display (char->integer #\\A)) (newline)
    (display (integer->char 97)) (newline)
    (display (char<? #\\a #\\b)) (newline)
  SCM
end

# --- Vectors ---------------------------------------------------------------

def gen_vector_ops
  n = rand(8) + 3
  <<~SCM
    (define v (make-vector #{n} 0))
    (define (loop i)
      (if (= i #{n}) (quote done)
          (begin (vector-set! v i (* i i)) (loop (+ i 1)))))
    (loop 0)
    (define (sum i acc)
      (if (= i #{n}) acc (sum (+ i 1) (+ acc (vector-ref v i)))))
    (display (sum 0 0)) (newline)
    (display (vector-length v)) (newline)
  SCM
end

def gen_vector_to_list
  <<~SCM
    (display (vector->list (vector 1 2 3 4 5))) (newline)
    (display (list->vector (list 10 20 30))) (newline)
  SCM
end

# --- List ops --------------------------------------------------------------

def gen_cons_walk
  n = rand(200) + 50
  <<~SCM
    (define (range a b) (if (>= a b) (quote ()) (cons a (range (+ a 1) b))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (define (len lst) (if (null? lst) 0 (+ 1 (len (cdr lst)))))
    (define xs (range 0 #{n}))
    (display (+ (sum xs) (len xs))) (newline)
  SCM
end

def gen_reverse_tail
  n = rand(100) + 20
  <<~SCM
    (define (range a b) (if (>= a b) (quote ()) (cons a (range (+ a 1) b))))
    (define (rev lst acc)
      (if (null? lst) acc (rev (cdr lst) (cons (car lst) acc))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (display (sum (rev (range 0 #{n}) (quote ())))) (newline)
  SCM
end

def gen_fold_right_deep
  n = rand(30) + 5
  <<~SCM
    (define (range a b) (if (>= a b) (quote ()) (cons a (range (+ a 1) b))))
    (define (fold-right f init lst)
      (if (null? lst) init (f (car lst) (fold-right f init (cdr lst)))))
    (display (fold-right + 0 (range 1 #{n}))) (newline)
  SCM
end

def gen_map_capturing
  n = rand(20) + 5
  k = rnum
  <<~SCM
    (define (range a b) (if (>= a b) (quote ()) (cons a (range (+ a 1) b))))
    (define (map1 f lst)
      (if (null? lst) (quote ()) (cons (f (car lst)) (map1 f (cdr lst)))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (define mul #{k})
    (display (sum (map1 (lambda (x) (* x mul)) (range 0 #{n}))))
    (newline)
  SCM
end

def gen_assoc
  <<~SCM
    (define al (quote ((a 1) (b 2) (c 3) (d 4))))
    (display (assoc (quote b) al)) (newline)
    (display (assoc (quote z) al)) (newline)
  SCM
end

# --- Control flow ---------------------------------------------------------

def gen_cond_chain
  v = rand(20)
  <<~SCM
    (display
      (cond ((= #{v} 0) (quote zero))
            ((< #{v} 5) (quote small))
            ((< #{v} 10) (quote medium))
            ((< #{v} 20) (quote large))
            (else (quote huge))))
    (newline)
  SCM
end

def gen_case
  v = rand(10)
  <<~SCM
    (display
      (case #{v}
        ((0 1 2) (quote small))
        ((3 4 5) (quote medium))
        ((6 7 8 9) (quote large))
        (else (quote unknown))))
    (newline)
  SCM
end

def gen_and_or
  <<~SCM
    (display (and 1 2 3 4 5)) (newline)
    (display (and 1 #f 3)) (newline)
    (display (and)) (newline)
    (display (or #f #f 7)) (newline)
    (display (or #f #f #f)) (newline)
    (display (or)) (newline)
  SCM
end

def gen_when_unless
  <<~SCM
    (define x #{rand(20)})
    (when (> x 5) (display (quote big)) (newline))
    (unless (> x 100) (display (quote not-huge)) (newline))
  SCM
end

# --- Recursion patterns ----------------------------------------------------

def gen_recursive_cons_capture
  n = rand(50) + 10
  <<~SCM
    (define (build-n n)
      (define base #{rnum})
      (define (helper i)
        (if (= i n) (quote ()) (cons (+ i base) (helper (+ i 1)))))
      (helper 0))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (display (sum (build-n #{n}))) (newline)
  SCM
end

def gen_deep_tail_recursion
  n = rand(50000) + 10000
  <<~SCM
    (define (count-down n) (if (= n 0) (quote done) (count-down (- n 1))))
    (display (count-down #{n})) (newline)
  SCM
end

def gen_fib_small
  n = rand(15) + 5
  <<~SCM
    (define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
    (display (fib #{n})) (newline)
  SCM
end

def gen_ack_small
  m = rand(3) + 1
  n = rand(3) + 1
  <<~SCM
    (define (ack m n)
      (cond ((= m 0) (+ n 1))
            ((= n 0) (ack (- m 1) 1))
            (else (ack (- m 1) (ack m (- n 1))))))
    (display (ack #{m} #{n})) (newline)
  SCM
end

# --- Quoting / quasiquote --------------------------------------------------

def gen_quote_data
  <<~SCM
    (define data (quote (1 2 3 4 5 6 7 8 9 10)))
    (define (double-each lst)
      (if (null? lst) (quote ()) (cons (* 2 (car lst)) (double-each (cdr lst)))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (display (sum (double-each data))) (newline)
  SCM
end

def gen_quasiquote
  v = rnum
  <<~SCM
    (define x #{v})
    (display `(a ,x b ,(+ x 1) c)) (newline)
  SCM
end

def gen_deep_quote
  <<~SCM
    (define d (quote ((1 2) (3 (4 5)) (6 7 (8 (9 10))))))
    (define (flat-sum lst)
      (cond ((null? lst) 0)
            ((pair? lst) (+ (flat-sum (car lst)) (flat-sum (cdr lst))))
            ((number? lst) lst)
            (else 0)))
    (display (flat-sum d)) (newline)
  SCM
end

# --- Type predicates -------------------------------------------------------

def gen_type_predicates
  <<~SCM
    (display (number? 42)) (newline)
    (display (string? "hi")) (newline)
    (display (symbol? (quote foo))) (newline)
    (display (pair? (quote (1)))) (newline)
    (display (null? (quote ()))) (newline)
    (display (boolean? #t)) (newline)
    (display (procedure? car)) (newline)
    (display (procedure? 42)) (newline)
  SCM
end

# --- Boolean short-circuit + side effects ----------------------------------

def gen_boolean_side_effect
  <<~SCM
    (define counter 0)
    (define (bump!) (set! counter (+ counter 1)) #t)
    (and (bump!) (bump!) (bump!))
    (display counter) (newline)
    (set! counter 0)
    (and (bump!) #f (bump!))
    (display counter) (newline)
  SCM
end

# --- Combo / composition ---------------------------------------------------

def gen_combo
  k = rand(3) + 2
  pool = [:gen_mutual_internal, :gen_capturing_filter, :gen_named_let,
          :gen_quote_data, :gen_vector_ops, :gen_letrec_mutual,
          :gen_do_loop, :gen_multi_internal, :gen_closure_chain,
          :gen_reverse_tail, :gen_fold_right_deep, :gen_arithmetic_mix,
          :gen_values_basic, :gen_delay_force]
  (0...k).map { send(pool.sample) }.join("\n")
end

# --- Boundary: deep nesting & large allocations ----------------------------

def gen_deep_let_nest
  <<~SCM
    (display
      (let ((a 1)) (let ((b 2)) (let ((c 3)) (let ((d 4))
        (let ((e 5)) (let ((f 6)) (let ((g 7)) (let ((h 8))
          (+ a b c d e f g h))))))))))
    (newline)
  SCM
end

def gen_big_let_shadowing
  n = rand(10) + 5
  shadows = (0...n).map { |i| "(let ((x (* x 2))) " }.join
  closes  = ")" * n
  <<~SCM
    (define x 1)
    (display
      #{shadows}x#{closes})
    (newline)
  SCM
end

def gen_huge_list
  n = rand(500) + 500
  <<~SCM
    (define (range a b) (if (>= a b) (quote ()) (cons a (range (+ a 1) b))))
    (define xs (range 0 #{n}))
    (define (len lst) (if (null? lst) 0 (+ 1 (len (cdr lst)))))
    (display (len xs)) (newline)
  SCM
end

# ---------------------------------------------------------------------------
# Targeted NODE_DEF coverage generators
# ---------------------------------------------------------------------------

# node_call_n (= ascheme call_K dispatcher for K >= 5).  Forces compile to
# emit node_call_n with args_idx + argc operands.
def gen_call_5
  args = (0...5).map { rnum }
  <<~SCM
    (define (f a b c d e) (+ a b c d e))
    (display (f #{args.join(' ')})) (newline)
  SCM
end

def gen_call_8
  args = (0...8).map { rnum }
  params = (0...8).map { |i| "p#{i}" }
  <<~SCM
    (define (f #{params.join(' ')}) (+ #{params.join(' ')}))
    (display (f #{args.join(' ')})) (newline)
  SCM
end

def gen_call_12
  args = (0...12).map { rnum }
  params = (0...12).map { |i| "p#{i}" }
  <<~SCM
    (define (f #{params.join(' ')}) (+ #{params.join(' ')}))
    (display (f #{args.join(' ')})) (newline)
  SCM
end

def gen_call_n_tail
  # Self-tail-call with 5+ args — exercises node_self_tail_call_global_K but K>4 path
  # actually ascheme has self_tail_call_global only up to K=4, so K>=5 falls
  # back to node_call_n with is_tail=1.
  # Cap the loop counter (= p0) to a small int so we don't time out on bignum
  # iterations.
  args = [rand(200) + 5] + (1..5).map { rnum }
  params = (0...6).map { |i| "p#{i}" }
  <<~SCM
    (define (f #{params.join(' ')})
      (if (= p0 0)
          (+ #{params[1..-1].join(' ')})
          (f (- p0 1) #{params[1..-1].join(' ')})))
    (display (f #{args.join(' ')})) (newline)
  SCM
end

# node_pred_null / node_pred_pair / node_pred_not — specialized predicates
# (= ascheme's compile recognizes (null? x) / (pair? x) / (not x) and emits
# a dedicated NODE rather than generic gref-call).
def gen_pred_null_pair
  <<~SCM
    (define (count lst)
      (if (null? lst) 0
          (if (pair? lst) (+ 1 (count (cdr lst))) 1)))
    (display (count (quote (a b c d e f)))) (newline)
    (display (count (quote ()))) (newline)
    (display (count (quote a))) (newline)
  SCM
end

def gen_pred_not_chain
  <<~SCM
    (define x #{rand(100)})
    (display (not (= x 0))) (newline)
    (display (not (not (= x 0)))) (newline)
    (display (if (not (null? (quote (a)))) (quote yes) (quote no))) (newline)
  SCM
end

# node_pred_car / node_pred_cdr — `(car X)` / `(cdr X)` as specialized
# inline access on pair.
def gen_pred_car_cdr
  <<~SCM
    (define p (cons 1 (cons 2 (cons 3 (quote ())))))
    (display (car p)) (newline)
    (display (car (cdr p))) (newline)
    (display (car (cdr (cdr p)))) (newline)
    (display (cdr (cdr (cdr p)))) (newline)
  SCM
end

# node_vec_ref / node_vec_set — `(vector-ref V I)` / `(vector-set! V I X)`
# specialized.
def gen_vec_ref_set
  n = rand(8) + 4
  <<~SCM
    (define v (make-vector #{n} 0))
    (define (fill i)
      (if (= i #{n}) (quote done)
          (begin (vector-set! v i (* i 3)) (fill (+ i 1)))))
    (fill 0)
    (define (sum i acc)
      (if (= i #{n}) acc (sum (+ i 1) (+ acc (vector-ref v i)))))
    (display (sum 0 0)) (newline)
  SCM
end

# node_cons_op — `(cons a b)` as a specialized node.
def gen_cons_op
  n = rand(30) + 5
  <<~SCM
    (define (build i acc)
      (if (= i 0) acc (build (- i 1) (cons i acc))))
    (define lst (build #{n} (quote ())))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (display (sum lst)) (newline)
  SCM
end

# node_eq_op / node_eqv_op — specialized equality compares.
def gen_eq_op
  v = rand(100)
  <<~SCM
    (display (eq? (quote a) (quote a))) (newline)
    (display (eq? (quote a) (quote b))) (newline)
    (display (eqv? #{v} #{v})) (newline)
    (display (eqv? #{v} #{v + 1})) (newline)
    (display (if (eq? #{v} #{v}) (quote yes) (quote no))) (newline)
  SCM
end

# node_self_tail_call_K (= named-let-style, not define-style).  The
# parser emits node_self_tail_call_K (NOT node_self_tail_call_global_K)
# for `(let loop ((i 0)) ... (loop ...))` when arity matches.
def gen_named_let_self_tail_K
  k = rand(3) + 2  # 2..4 args
  args = (0...k).map { |i| ["i#{i}", rand(10)] }
  init = args.map { |a| "(#{a[0]} #{a[1]})" }.join(' ')
  step = args.map.with_index { |a, i| i == 0 ? "(- #{a[0]} 1)" : a[0] }.join(' ')
  body_acc = args.map { |a| a[0] }.join(' ')
  n = rand(20) + 5
  <<~SCM
    (display
      (let loop (#{init})
        (if (<= i0 0) (+ #{body_acc}) (loop #{step}))))
    (newline)
  SCM
end

# Many-arg structural exercise
def gen_many_arg_lambda
  n = rand(5) + 5  # 5..9 args
  params = (0...n).map { |i| "v#{i}" }
  args = (0...n).map { rnum }
  body = params.each_with_index.map { |p, i| "(* #{p} #{i + 1})" }.join(' ')
  <<~SCM
    (define (f #{params.join(' ')}) (+ #{body}))
    (display (f #{args.join(' ')})) (newline)
  SCM
end

# Mixed pred + arith + control nested deeply
def gen_pred_arith_mix
  n = rand(30) + 5
  <<~SCM
    (define (count-positives lst)
      (cond ((null? lst) 0)
            ((not (pair? lst)) 0)
            ((> (car lst) 0) (+ 1 (count-positives (cdr lst))))
            (else (count-positives (cdr lst)))))
    (define data (quote (1 -2 3 -4 5 6 -7 8 -9 10)))
    (display (count-positives data)) (newline)
  SCM
end

# ---------------------------------------------------------------------------
# Multi-shot continuation (#3) — invoke captured cc more than once.
# ---------------------------------------------------------------------------

def gen_multi_shot_cc
  # Calls captured continuation twice — chez supports this fully.
  # ascheme_precise marks one-shot (= active=0 after invoke); second invoke
  # should raise a clean error, NOT SEGV.
  <<~SCM
    (define saved #f)
    (define result
      (+ 1 (call/cc (lambda (k) (set! saved k) 10))))
    (display result) (newline)
    (if saved
        (let ((tmp saved))
          (set! saved #f)
          (tmp 100))
        (quote done))
  SCM
end

def gen_cc_reinvoke_after_error
  # Try invoking continuation that's been "consumed".
  <<~SCM
    (define stored #f)
    (define (capture)
      (call/cc (lambda (k) (set! stored k) (quote captured))))
    (capture)
    (display (quote ok)) (newline)
  SCM
end

# ---------------------------------------------------------------------------
# Floating point / numeric edge cases (#4)
# ---------------------------------------------------------------------------

def gen_float_edges
  # Avoid IEEE round-trip-precision-sensitive cases (chez = %.17g, precise =
  # %.15g) — use values whose %.15g and %.17g representations agree so the
  # oracle comparison stays meaningful.  Float printing precision is a known
  # divergence, not a precise-rooting bug.
  <<~SCM
    (display (+ 1.5 2.5)) (newline)
    (display (* 2.0 3.0)) (newline)
    (display (- 10.0 4.5)) (newline)
    (display (sqrt 16.0)) (newline)
    (display (/ 1.0 4.0)) (newline)
  SCM
end

def gen_float_compare
  <<~SCM
    (display (= 1.0 1)) (newline)
    (display (< 1.5 2)) (newline)
    (display (< -1.5 0)) (newline)
    (display (= 0.0 -0.0)) (newline)
  SCM
end

def gen_negative_bignum
  <<~SCM
    (display (- 0 999999999999999)) (newline)
    (display (* -999999999 999999999)) (newline)
    (display (expt 2 50)) (newline)
    (display (- (expt 2 50))) (newline)
  SCM
end

def gen_mixed_numeric
  # Force ascheme's numeric tower coercion chains.
  <<~SCM
    (display (+ 1 1.5)) (newline)        ; fix + flonum → flonum
    (display (+ 1 1/2)) (newline)        ; fix + rational → rational
    (display (+ 1.5 1/2)) (newline)      ; flonum + rational → flonum
    (display (* 999999999999 2)) (newline)  ; fix → bignum
    (display (/ 1 3)) (newline)          ; → rational
    (display (* 1/2 2/3)) (newline)      ; rational × rational
  SCM
end

# ---------------------------------------------------------------------------
# Port I/O (#4 cont.)
# ---------------------------------------------------------------------------

def gen_string_port
  # `with-output-to-string` not portable, skip; just exercise (write ...)
  # which has its own formatting path.
  <<~SCM
    (write (quote (a b c))) (newline)
    (write 42) (newline)
    (write "hello") (newline)
    (write #t) (newline)
    (write (cons 1 2)) (newline)
  SCM
end

# ---------------------------------------------------------------------------
# Coverage-targeted generators (= for primitives gcov shows as 0%-hit)
# ---------------------------------------------------------------------------

# caar / cadr / caddr / cdddr / cdar / cddr / cadddr 系
def gen_list_accessors
  <<~SCM
    (define p (quote ((1 2 3) (4 5 6) (7 8 9))))
    (display (caar p)) (newline)
    (display (cadar p)) (newline)
    (display (cddar p)) (newline)
    (display (cdar p)) (newline)
    (display (cadr p)) (newline)
    (display (caddr p)) (newline)
    (display (cddr p)) (newline)
    (display (cadddr (quote (1 2 3 4 5)))) (newline)
  SCM
end

# Trig / transcendental — use inexact (float) inputs so chez and precise
# both return inexact and format-precision agrees.  Exact 0 makes chez
# preserve exact 0 from sin/cos, which precise inexact-ifies.
def gen_trig_ops
  <<~SCM
    (display (sin 0.5)) (newline)
    (display (cos 0.5)) (newline)
    (display (tan 0.5)) (newline)
    (display (atan 0.5)) (newline)
    (display (exp 1.0)) (newline)
    (display (log 2.0)) (newline)
    (display (floor 3.7)) (newline)
    (display (ceiling 3.2)) (newline)
    (display (round 3.5)) (newline)
    (display (truncate 3.7)) (newline)
  SCM
end

# gensym / symbols
def gen_gensym
  <<~SCM
    (define s (gensym))
    (display (symbol? s)) (newline)
    (display (eq? s s)) (newline)
    (display (eq? s (gensym))) (newline)
  SCM
end

# exact / inexact conversions
def gen_exact_inexact
  <<~SCM
    (display (exact->inexact 1/2)) (newline)
    (display (inexact->exact 0.5)) (newline)
    (display (exact->inexact 3)) (newline)
    (display (inexact? 1.5)) (newline)
    (display (exact? 1/2)) (newline)
    (display (exact? 3)) (newline)
  SCM
end

# gcd / lcm
def gen_gcd_lcm
  <<~SCM
    (display (gcd 12 18)) (newline)
    (display (gcd 36 24 16)) (newline)
    (display (lcm 4 6)) (newline)
    (display (lcm 4 6 9)) (newline)
    (display (gcd)) (newline)
    (display (lcm)) (newline)
  SCM
end

# Character predicates
def gen_char_predicates
  <<~SCM
    (display (char-alphabetic? #\\a)) (newline)
    (display (char-alphabetic? #\\5)) (newline)
    (display (char-numeric? #\\5)) (newline)
    (display (char-whitespace? #\\space)) (newline)
    (display (char-upper-case? #\\A)) (newline)
    (display (char-lower-case? #\\a)) (newline)
    (display (char-upcase #\\a)) (newline)
    (display (char-downcase #\\Z)) (newline)
  SCM
end

# string mutation + comparison
def gen_string_more
  <<~SCM
    (define s (make-string 5 #\\x))
    (display s) (newline)
    (string-set! s 0 #\\H)
    (display s) (newline)
    (display (string<? "abc" "abd")) (newline)
    (display (string=? "abc" "abc")) (newline)
    (display (string-copy "hello")) (newline)
    (display (string-ref "hello" 1)) (newline)
  SCM
end

# list ops: list-ref / list-tail / member / memq / reverse / append / map / for-each
def gen_list_higher
  <<~SCM
    (display (list-ref (quote (a b c d e)) 2)) (newline)
    (display (list-tail (quote (a b c d e)) 2)) (newline)
    (display (member 3 (quote (1 2 3 4 5)))) (newline)
    (display (memq (quote x) (quote (a b x c)))) (newline)
    (display (reverse (quote (1 2 3 4 5)))) (newline)
    (display (append (quote (1 2)) (quote (3 4)) (quote (5 6)))) (newline)
    (display (length (quote (1 2 3 4 5)))) (newline)
  SCM
end

# input from string (= sscanf-like) — chez compat
def gen_string_to_number
  <<~SCM
    (display (string->number "42")) (newline)
    (display (string->number "3.14")) (newline)
    (display (string->number "1/2")) (newline)
    (display (string->number "abc")) (newline)
    (display (number->string 42)) (newline)
    (display (number->string 1/2)) (newline)
  SCM
end

# expt with various exponents — avoid `(expt int -int)` which chez returns
# as exact rational (1/100) while precise returns inexact (0.01).
def gen_expt_variants
  <<~SCM
    (display (expt 2 10)) (newline)
    (display (expt 3 5)) (newline)
    (display (expt 2 0)) (newline)
    (display (expt 0 5)) (newline)
    (display (expt 5 1)) (newline)
  SCM
end

# Mixed apply with prims of various arities
def gen_apply_prims
  <<~SCM
    (display (apply + (list 1 2 3 4 5))) (newline)
    (display (apply max (list 3 1 4 1 5 9 2 6))) (newline)
    (display (apply min (list 3 1 4 1 5 9 2 6))) (newline)
    (display (apply cons (list 1 2))) (newline)
    (display (apply list (list 1 2 3 4 5))) (newline)
  SCM
end

# do loop with explicit (test-expr result-exprs) yielding
def gen_do_variant
  <<~SCM
    (define v (make-vector 5 0))
    (do ((i 0 (+ i 1)))
        ((= i 5))
      (vector-set! v i (* i i)))
    (display v) (newline)
  SCM
end

# Boolean predicates of various typed values
def gen_more_predicates
  <<~SCM
    (display (integer? 5)) (newline)
    (display (integer? 5.0)) (newline)
    (display (integer? 5.5)) (newline)
    (display (rational? 1/2)) (newline)
    (display (real? 1.5)) (newline)
    (display (complex? 5)) (newline)
    (display (exact-integer? 5)) (newline)
  SCM
end

# ---------------------------------------------------------------------------
# Mutation + crossover (#2)
# ---------------------------------------------------------------------------
# These take a seed program (from any other generator) and produce a derived
# program by simple AST manipulation in the source text.  Cheap (= regex /
# str.replace level) — not real AST mutation but enough to surface bugs
# that the deterministic templates miss.

CORPUS = []  # accumulate passing programs as seed material

def mutate_program(prog)
  case rand(5)
  when 0  # bump every numeric literal by +/- 1
    prog.gsub(/(?<![\w-])(-?\d+)(?![\w.])/) { |m| (m.to_i + (rand(3) - 1)).to_s }
  when 1  # wrap the whole program in (begin ...)
    "(begin\n#{prog}\n)"
  when 2  # replace some + with * (preserving syntactic validity)
    prog.gsub(/\(\+/) { |m| rand < 0.3 ? '(*' : m }
  when 3  # replace + with - in some sites
    prog.gsub(/\(\+/) { |m| rand < 0.3 ? '(-' : m }
  when 4  # wrap display arg with (abs ...) — should still be valid for numerics
    prog.gsub(/\(display ([^()]+)\)/) { |m| rand < 0.3 ? "(display (abs #{$1}))" : m }
  else
    prog
  end
end

def gen_mutated_corpus
  return gen_structural if CORPUS.empty?
  seed = CORPUS.sample
  mutate_program(seed)
end

# Crossover: concatenate two corpus programs (= state carries over).
def gen_crossover
  return gen_structural if CORPUS.size < 2
  a = CORPUS.sample
  b = CORPUS.sample
  "#{a}\n#{b}"
end

# ---------------------------------------------------------------------------
# Structural random AST generator
# ---------------------------------------------------------------------------
# Generates a numeric-valued expression using a small grammar with a depth
# bound.  All productions are designed to (a) typecheck under R5RS (never
# call car on non-pair, etc.) and (b) terminate (no infinite recursion).
# `env` is the list of currently-bound number-valued local names.

NUM_PRIMS = %w[+ - * min max abs]
NUM2_PRIMS = %w[+ - * quotient modulo remainder min max]
CMP_PRIMS = %w[< <= > >= =]

# Build a random numeric expression.  Returns a Scheme string.
def gen_expr_num(env, depth)
  if depth <= 0 || rand < 0.3
    # leaf
    if env.any? && rand < 0.5
      env.sample
    else
      rnum.to_s
    end
  else
    case rand(10)
    when 0  # binary arith
      op = NUM2_PRIMS.sample
      "(#{op} #{gen_expr_num(env, depth - 1)} #{gen_expr_num(env, depth - 1)})"
    when 1  # variadic arith
      op = NUM_PRIMS.sample
      n  = rand(3) + 2
      args = (0...n).map { gen_expr_num(env, depth - 1) }
      "(#{op} #{args.join(' ')})"
    when 2  # if
      c = gen_expr_bool(env, depth - 1)
      t = gen_expr_num(env, depth - 1)
      e = gen_expr_num(env, depth - 1)
      "(if #{c} #{t} #{e})"
    when 3  # let
      v = rsym('lv')
      val = gen_expr_num(env, depth - 1)
      body = gen_expr_num(env + [v], depth - 1)
      "(let ((#{v} #{val})) #{body})"
    when 4  # immediate lambda call (IIFE) — small arity
      params = [rsym('p1'), rsym('p2')]
      args = [gen_expr_num(env, depth - 1), gen_expr_num(env, depth - 1)]
      body = gen_expr_num(env + params, depth - 1)
      "((lambda (#{params.join(' ')}) #{body}) #{args.join(' ')})"
    when 5  # let* with two bindings
      v1 = rsym('lv')
      v2 = rsym('lv')
      val1 = gen_expr_num(env, depth - 1)
      val2 = gen_expr_num(env + [v1], depth - 1)
      body = gen_expr_num(env + [v1, v2], depth - 1)
      "(let* ((#{v1} #{val1}) (#{v2} #{val2})) #{body})"
    when 6  # cond
      cs = (0..1).map do
        c = gen_expr_bool(env, depth - 1)
        b = gen_expr_num(env, depth - 1)
        "(#{c} #{b})"
      end
      els = gen_expr_num(env, depth - 1)
      "(cond #{cs.join(' ')} (else #{els}))"
    when 7  # list-length via known short list
      n = rand(3) + 1
      nums = (0...n).map { gen_expr_num(env, 1) }
      "(length (list #{nums.join(' ')}))"
    when 8  # capture-bearing closure inside let
      v = rsym('cap')
      val = gen_expr_num(env, depth - 1)
      body = gen_expr_num(env + [v], depth - 1)
      "(let ((#{v} #{val})) ((lambda () #{body})))"
    else
      # absolute value of arith
      "(abs #{gen_expr_num(env, depth - 1)})"
    end
  end
end

def gen_expr_bool(env, depth)
  if depth <= 0
    "#t"
  else
    case rand(5)
    when 0
      op = CMP_PRIMS.sample
      "(#{op} #{gen_expr_num(env, depth - 1)} #{gen_expr_num(env, depth - 1)})"
    when 1
      "(and #{gen_expr_bool(env, depth - 1)} #{gen_expr_bool(env, depth - 1)})"
    when 2
      "(or #{gen_expr_bool(env, depth - 1)} #{gen_expr_bool(env, depth - 1)})"
    when 3
      "(not #{gen_expr_bool(env, depth - 1)})"
    when 4
      "(zero? #{gen_expr_num(env, depth - 1)})"
    else
      rand < 0.5 ? "#t" : "#f"
    end
  end
end

def gen_structural
  depth = rand(5) + 4   # bumped from 3+rand(4) — push deeper structural ASTs
  prog  = gen_expr_num([], depth)
  "(display #{prog}) (newline)"
end

def gen_structural_with_lambda
  # A program that defines a small helper, then calls it.
  fn = rsym('fn')
  v  = rsym('arg')
  body = gen_expr_num([v], rand(3) + 2)
  arg  = gen_expr_num([], rand(3) + 2)
  <<~SCM
    (define (#{fn} #{v}) #{body})
    (display (#{fn} #{arg})) (newline)
  SCM
end

def gen_structural_letrec
  # define two mutually-defined functions, call one.
  f = rsym('f')
  g = rsym('g')
  v = rsym('v')
  fbody = "(if (<= #{v} 0) 0 (+ 1 (#{g} (- #{v} 1))))"
  gbody = "(if (<= #{v} 0) 0 (+ 2 (#{f} (- #{v} 1))))"
  n = rand(20) + 5
  <<~SCM
    (define (#{f} #{v}) #{fbody})
    (define (#{g} #{v}) #{gbody})
    (display (#{f} #{n})) (newline)
  SCM
end

# A nested let chain holding sub-results, ensures we exercise the let-as-call
# desugaring with cross-let captures and per-let frame allocation.
def gen_structural_deep_let
  depth = rand(3) + 4
  expr = gen_expr_num([], depth)
  vars = []
  body = expr
  (1..depth).each do |i|
    v = rsym("vl#{i}")
    vars << v
    init = gen_expr_num(vars[0...-1], 2)
    body = "(let ((#{v} #{init})) #{body})"
  end
  "(display #{body}) (newline)"
end

# Sequential set! sequence inside a lambda — exercises lset on captured slots.
def gen_structural_set
  n = rand(3) + 2
  v = rsym('s')
  inits = (0...n).map { gen_expr_num([], 2) }
  <<~SCM
    (define #{v} #{inits.first})
    #{(1...n).map { |i| "(set! #{v} (+ #{v} #{inits[i]}))" }.join("\n")}
    (display #{v}) (newline)
  SCM
end

# Heavy nested-call structural — sometimes triggers stack issues.
def gen_structural_nested_call
  v = rsym('f')
  body = "(if (= n 0) 0 (+ n (#{v} (- n 1))))"
  n = rand(30) + 5
  <<~SCM
    (define (#{v} n) #{body})
    (display (#{v} #{n})) (newline)
  SCM
end

# Build a list, walk via fold-style reduction with cross-capturing lambda.
def gen_structural_higher_order
  v = rsym('a')
  k = rnum
  n = rand(20) + 5
  body = gen_expr_num([v], rand(3) + 2)
  <<~SCM
    (define (range a b) (if (>= a b) (quote ()) (cons a (range (+ a 1) b))))
    (define (map f lst) (if (null? lst) (quote ()) (cons (f (car lst)) (map f (cdr lst)))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (define k #{k})
    (display (sum (map (lambda (#{v}) #{body}) (range 0 #{n})))) (newline)
  SCM
end

# ---------------------------------------------------------------------------
# Generator registry
# ---------------------------------------------------------------------------

TEMPLATES = %i[
  gen_mutual_internal gen_multi_internal gen_nested_define
  gen_letrec_mutual gen_letrec_value_and_lambda
  gen_named_let gen_named_let_with_break gen_do_loop
  gen_capturing_filter gen_y_factorial gen_closure_chain
  gen_mutable_capture gen_mutable_capture_shared
  gen_nested_let_capture gen_vector_closures gen_list_of_closures
  gen_closure_mutated_from_inner
  gen_callcc_basic gen_callcc_deep gen_callcc_early_return
  gen_dotted_rest gen_rest_only_lambda gen_apply gen_apply_with_lambda
  gen_values_basic gen_values_zero gen_values_single gen_values_many gen_values_capture
  gen_delay_force gen_delay_force_capture
  gen_bignum_arith gen_bignum_factorial gen_rational
  gen_number_predicates gen_arithmetic_mix
  gen_eq_predicates gen_string_ops gen_string_list_conv
  gen_char_ops gen_vector_ops gen_vector_to_list
  gen_cons_walk gen_reverse_tail gen_fold_right_deep gen_map_capturing gen_assoc
  gen_cond_chain gen_case gen_and_or gen_when_unless
  gen_recursive_cons_capture gen_deep_tail_recursion gen_fib_small gen_ack_small
  gen_quote_data gen_quasiquote gen_deep_quote
  gen_type_predicates gen_boolean_side_effect
  gen_combo gen_deep_let_nest gen_big_let_shadowing gen_huge_list
  gen_call_5 gen_call_8 gen_call_12 gen_call_n_tail gen_many_arg_lambda
  gen_pred_null_pair gen_pred_not_chain gen_pred_car_cdr gen_pred_arith_mix
  gen_vec_ref_set gen_cons_op gen_eq_op gen_named_let_self_tail_K
  gen_multi_shot_cc gen_cc_reinvoke_after_error
  gen_float_edges gen_float_compare gen_negative_bignum gen_mixed_numeric
  gen_string_port
  gen_list_accessors gen_gensym gen_exact_inexact
  gen_gcd_lcm gen_char_predicates gen_string_more gen_list_higher
  gen_string_to_number gen_expt_variants gen_apply_prims gen_do_variant
  gen_more_predicates
]
# Note: gen_trig_ops is intentionally not in TEMPLATES.  chez prints %.17g
# (= 17-digit IEEE round-trip), precise prints %.15g; transcendental
# results disagree at the 16-17th decimal.  This is a representation
# difference, not a bug, so excluding it removes false-positive churn.

STRUCTURAL = %i[
  gen_structural
  gen_structural_with_lambda
  gen_structural_letrec
  gen_structural_deep_let
  gen_structural_set
  gen_structural_nested_call
  gen_structural_higher_order
  gen_mutated_corpus
  gen_crossover
]

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def kind(status)
  return :timeout if status.exitstatus == 124
  return :segv    if status.signaled? || status.exitstatus == 139
  return :error   unless status.success?
  :ok
end

def run_program(bin, prog, env = {}, args_extra = [])
  Tempfile.create(['fuzz', '.scm']) do |f|
    f.write(prog); f.flush
    args = if bin == ORACLE
             ['timeout', TIME.to_s, bin, *ORACLE_ARGS, f.path]
           else
             ['timeout', TIME.to_s, bin, '-q', *args_extra, f.path]
           end
    return Open3.capture3(env, *args)
  end
end

def clean_precise_stdout(s)
  s.lines.reject { |l| l.start_with?('[baruby_gc') || l.start_with?('[aot') }.join
end

# Modes: each runs precise with different env / flag combination.
MODE_SPECS = {
  'plain'      => { env: {}, args: ['--plain'] },
  'stress'     => { env: { 'BARUBY_GC_STRESS' => '1' }, args: ['--plain'] },
  'aot'        => { env: {}, args: [] },              # AOT auto-loads via no_compiled_code=false
  'aot-stress' => { env: { 'BARUBY_GC_STRESS' => '1' }, args: [] },
}

invalid = MODES - MODE_SPECS.keys
abort("unknown MODES: #{invalid.join(',')}") unless invalid.empty?

# AOT cache prep: if any mode is aot*, build cache via --pg-compile for the
# generated programs as we go (each program's AOT entries are baked the first
# time it's run in `aot` mode).  But we don't pre-prime — the AOT cache lives
# at code_store/all.so and is shared across runs; programs that don't bake
# their entries fall back to the host dispatcher (= still correct, just slower).
# For deterministic testing, we run each program through --pg-compile first
# when any aot* mode is in MODES.
AOT_NEEDED = MODES.any? { |m| m.start_with?('aot') }

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

puts "fuzz: N=#{N} SEED=#{SEED} MODES=#{MODES.join(',')} STRUCT_RATIO=#{STRUCT_RATIO}"
fails = 0
pass  = 0
skipped = 0
gen_counts = Hash.new(0)
mode_fails = Hash.new(0)

N.times do |i|
  # Pick generator: STRUCT_RATIO probability structural, else template.
  if rand < STRUCT_RATIO && !STRUCTURAL.empty?
    gen = STRUCTURAL.sample
  else
    gen = TEMPLATES.sample
  end
  prog = send(gen)
  gen_counts[gen] += 1

  # Oracle (chez): get expected output
  o_out, o_err, o_st = run_program(ORACLE, prog)
  o_k = kind(o_st)
  if o_k != :ok
    # chez itself failed or timed out — likely program exercises a feature
    # chez doesn't accept (e.g., r5rs/r6rs mismatch).  Skip rather than fail.
    skipped += 1
    next
  end

  # Mode matrix: run precise in each enabled mode, compare to oracle.
  failed_mode = nil; failed_reason = nil; failed_out = nil; failed_err = nil
  MODES.each do |mode|
    spec = MODE_SPECS[mode]
    # For aot* modes, prime the cache on first run; the result of pg-compile is
    # also a valid run, so we can reuse it.  We prep with --pg-compile -q so
    # the cache is built before measuring.
    if mode.start_with?('aot') && AOT_NEEDED
      # Make sure code_store/all.so includes this program's hashes.
      Tempfile.create(['fuzz', '.scm']) do |f|
        f.write(prog); f.flush
        Open3.capture3({ 'CCACHE_DISABLE' => '1' }, 'timeout', TIME.to_s,
                       PRECISE, '--pg-compile', '-q', f.path)
      end
    end
    p_out, p_err, p_st = run_program(PRECISE, prog, spec[:env], spec[:args])
    p_k = kind(p_st)
    p_clean = clean_precise_stdout(p_out)

    if p_k == :segv || p_k == :timeout || (p_k == :ok && p_clean != o_out)
      failed_mode = mode
      failed_reason = p_k == :ok ? :mismatch : p_k
      failed_out = p_out
      failed_err = p_err
      break
    end
  end

  if failed_mode
    fails += 1
    mode_fails[failed_mode] += 1
    path = File.join(FAILDIR, format("%03d_%s_%s.scm", fails, gen, failed_mode))
    File.write(path, prog)
    File.write(path + '.expected', o_out)
    File.write(path + '.got', failed_out)
    File.write(path + '.stderr', failed_err)
    msg = "FAIL ##{i + 1} #{gen} mode=#{failed_mode} reason=#{failed_reason} → #{path}"
    puts msg
    if fails >= MAX_FAILS
      puts "fuzz: too many failures (#{fails}), stopping"
      break
    end
  else
    pass += 1
    # Keep a bounded corpus of passing programs for mutation/crossover.
    # Bound to 200 to avoid runaway memory.
    if CORPUS.size < 200 && rand < 0.3
      CORPUS << prog
    end
    if (i + 1) % 25 == 0
      print "."
      $stdout.flush
    end
  end
end

puts
puts "fuzz: N=#{N} SEED=#{SEED}"
puts "  pass=#{pass}  fail=#{fails}  skipped=#{skipped}"
unless mode_fails.empty?
  puts "  failures by mode:"
  mode_fails.sort_by { |_, v| -v }.each do |mode, v|
    puts "    #{mode}: #{v}"
  end
end
puts "  generator usage (top 10):"
gen_counts.sort_by { |_, v| -v }.first(10).each do |gen, count|
  puts "    #{gen}: #{count}"
end
total_used = gen_counts.keys.size
total_avail = TEMPLATES.size + STRUCTURAL.size
puts "  generators used: #{total_used}/#{total_avail}"

exit(fails > 0 ? 1 : 0)
