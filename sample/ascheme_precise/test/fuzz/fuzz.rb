#!/usr/bin/env ruby
# test/fuzz/fuzz.rb — random Scheme generator + stress-GC_BACKEND SEGV catcher.
#
# Strategy:
#   1. Generate small valid R5RS-ish programs from a template library that
#      stresses precise-rooting hot spots: internal defines, letrec, named-
#      let, call/cc with deep stacks, capturing closures, dotted-rest
#      lambdas, cons-heavy ops.
#   2. Each program prints a single deterministic value to stdout.
#   3. Run twice — once with the "oracle" (= libgc-based ../ascheme), once
#      with ascheme_precise under BARUBY_GC_BACKEND_STRESS=1 + the chosen backend.
#   4. Fail if: precise SEGVs / hangs / produces output != oracle.
#   5. On failure, dump the offending program to test/fuzz/fails/<n>.scm.
#
# Usage:
#   ruby test/fuzz/fuzz.rb [N] [GC_BACKEND=name]
#     N  — number of programs to generate (default 200)
#     GC_BACKEND — ascheme_precise GC_BACKEND backend (default copy_scramble; see Makefile)

require 'fileutils'
require 'open3'

N      = (ARGV[0] || "200").to_i
GC_BACKEND     = ENV['GC_BACKEND'] || 'copy_scramble'
TIME   = 15  # per-program timeout (sec)
SEED   = (ENV['SEED'] || Time.now.to_i).to_i
srand(SEED)

ROOT      = File.expand_path('../..', __dir__)
PRECISE   = File.join(ROOT, 'ascheme_precise')
# chez is the oracle of correctness (= R5RS-compliant native compiler).
# libgc-based ascheme shares the same interpreter core as ascheme_precise,
# so several semantic bugs surface in both and would silently pass under a
# co-shared oracle.
ORACLE    = ENV['ORACLE'] || 'scheme'      # chez "scheme --script"
ORACLE_ARGS = ['--script']
FAILDIR   = File.join(__dir__, 'fails')
FileUtils.mkdir_p(FAILDIR)

abort("missing binary: #{PRECISE}") unless File.executable?(PRECISE)

# ---------------------------------------------------------------------------
# Generators.  Each returns a self-contained Scheme program as a string.
# ---------------------------------------------------------------------------

# Random symbol name (so the test exercises lexical scoping)
def rsym(prefix = 'v')
  "#{prefix}#{rand(1000)}"
end

# Random small fixnum
def rnum
  case rand(4)
  when 0 then rand(100)
  when 1 then -rand(50)
  when 2 then rand(10)
  else        rand(1000)
  end
end

# Generator: nested internal defines + recursion (= the pattern that hit #182)
def gen_internal_defines
  v = rsym('v')
  n = rand(20) + 5
  acc = rsym('acc')
  helper = rsym('h')
  <<~SCM
    (define (#{rsym('f')})
      (define (#{helper} #{v} #{acc})
        (if (= #{v} 0) #{acc} (#{helper} (- #{v} 1) (+ #{acc} #{v}))))
      (#{helper} #{n} 0))
    (display (#{Regexp.last_match.nil? ? 'begin' : 'begin'} (display 'ok-) (display (- 0))))
  SCM
end

# Mutual recursion via internal defines
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

# Capturing lambda → filter
def gen_capturing_filter
  n  = rand(30) + 5
  k  = rand(5) + 2
  <<~SCM
    (define (filter p lst)
      (cond ((null? lst) '())
            ((p (car lst)) (cons (car lst) (filter p (cdr lst))))
            (else (filter p (cdr lst)))))
    (define (range a b) (if (>= a b) '() (cons a (range (+ a 1) b))))
    (define (count lst) (if (null? lst) 0 (+ 1 (count (cdr lst)))))
    (define (test threshold)
      (count (filter (lambda (x) (> x threshold)) (range 0 #{n}))))
    (display (test #{k})) (newline)
  SCM
end

# call/cc invoked from deep recursion
def gen_callcc_deep
  d = rand(100) + 20
  <<~SCM
    (define (test)
      (call/cc
        (lambda (k)
          (define (deep n)
            (if (= n 0) (k 'escaped) (deep (- n 1))))
          (deep #{d}))))
    (display (test)) (newline)
  SCM
end

# Named-let with self-tail-call
def gen_named_let
  n = rand(100) + 10
  <<~SCM
    (display
      (let loop ((i 0) (acc 0))
        (if (= i #{n}) acc (loop (+ i 1) (+ acc i)))))
    (newline)
  SCM
end

# letrec with mutual recursion in actual letrec form
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

# Y combinator-ish (= heavy capture + closure chains)
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

# Cons-heavy: build huge list, walk twice
def gen_cons_walk
  n = rand(200) + 50
  <<~SCM
    (define (range a b) (if (>= a b) '() (cons a (range (+ a 1) b))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (define (len lst) (if (null? lst) 0 (+ 1 (len (cdr lst)))))
    (define xs (range 0 #{n}))
    (display (+ (sum xs) (len xs))) (newline)
  SCM
end

# Dotted-rest lambda
def gen_dotted_rest
  vals = (0..rand(5)+1).map { rnum }
  <<~SCM
    (define (rest-fn first . rest)
      (cons first (length rest)))
    (display (rest-fn #{vals.join(' ')})) (newline)
  SCM
end

# Higher-order map with capturing lambda
def gen_map_capturing
  n = rand(20) + 5
  k = rnum
  <<~SCM
    (define (range a b) (if (>= a b) '() (cons a (range (+ a 1) b))))
    (define (map1 f lst)
      (if (null? lst) '() (cons (f (car lst)) (map1 f (cdr lst)))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (define mul #{k})
    (display (sum (map1 (lambda (x) (* x mul)) (range 0 #{n}))))
    (newline)
  SCM
end

# Quoted data + cons construction (= tests quote relocation + cons rooting)
def gen_quote_data
  <<~SCM
    (define data '(1 2 3 4 5 6 7 8 9 10))
    (define (double-each lst)
      (if (null? lst) '() (cons (* 2 (car lst)) (double-each (cdr lst)))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (display (sum (double-each data))) (newline)
  SCM
end

# Vectors (= different OBJ_TYPE under GC_BACKEND)
def gen_vector
  n = rand(20) + 5
  <<~SCM
    (define v (make-vector #{n} 0))
    (define (loop i)
      (if (= i #{n}) 'done
          (begin (vector-set! v i (* i i)) (loop (+ i 1)))))
    (loop 0)
    (define (sum i acc)
      (if (= i #{n}) acc (sum (+ i 1) (+ acc (vector-ref v i)))))
    (display (sum 0 0)) (newline)
  SCM
end

# String construction
def gen_string
  <<~SCM
    (define s "hello world")
    (display (string-length s)) (newline)
    (display (string-append s s)) (newline)
  SCM
end

# (do (...) (...) ...) loop — desugars to named-let
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

# case statement
def gen_case
  v = rand(10)
  <<~SCM
    (display
      (case #{v}
        ((0 1 2) 'small)
        ((3 4 5) 'medium)
        ((6 7 8 9) 'large)
        (else 'unknown)))
    (newline)
  SCM
end

# Multiple internal defines (= triggers letrec desugar with multiple bindings)
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

# Nested let with shadowing
def gen_let_shadowing
  n = rand(50) + 5
  <<~SCM
    (define x #{n})
    (display
      (let ((x (* x 2)))
        (let ((x (+ x 1)))
          (let ((x (- x x)))
            x))))
    (newline)
  SCM
end

# Recursion through multiple closures (= heavy capture chain)
def gen_closure_chain
  n = rand(10) + 3
  <<~SCM
    (define (make-adder k)
      (lambda (x) (+ x k)))
    (define (make-multiplier k)
      (lambda (x) (* x k)))
    (define f (make-adder #{n}))
    (define g (make-multiplier #{rand(5) + 2}))
    (display (g (f #{rand(20)}))) (newline)
  SCM
end

# apply with computed argument list
def gen_apply
  n = rand(5) + 2
  args = (0..n).map { rnum }
  <<~SCM
    (display (apply + (list #{args.join(' ')}))) (newline)
  SCM
end

# call/cc + closure escape
def gen_callcc_escape
  <<~SCM
    (define stored #f)
    (define (capture)
      (call/cc (lambda (k) (set! stored k) 'captured)))
    (capture)
    (display 'done) (newline)
  SCM
end

# tail-recursive list reverse (= stresses sframe alloc + tail-call frame reuse)
def gen_reverse
  n = rand(100) + 20
  <<~SCM
    (define (range a b) (if (>= a b) '() (cons a (range (+ a 1) b))))
    (define (rev lst acc)
      (if (null? lst) acc (rev (cdr lst) (cons (car lst) acc))))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (display (sum (rev (range 0 #{n}) '()))) (newline)
  SCM
end

# fold-right (= deeply nested non-tail recursion)
def gen_fold_right
  n = rand(30) + 5
  <<~SCM
    (define (range a b) (if (>= a b) '() (cons a (range (+ a 1) b))))
    (define (fold-right f init lst)
      (if (null? lst) init
          (f (car lst) (fold-right f init (cdr lst)))))
    (display (fold-right + 0 (range 1 #{n}))) (newline)
  SCM
end

# Sequential composition: run 2-4 generator outputs back-to-back to exercise
# state carry-over between top-level forms.
def gen_combo
  k = rand(3) + 2
  progs = (0...k).map { send([:gen_mutual_internal, :gen_capturing_filter,
                               :gen_named_let, :gen_quote_data, :gen_vector,
                               :gen_letrec_mutual, :gen_callcc_deep,
                               :gen_do_loop, :gen_multi_internal,
                               :gen_closure_chain, :gen_reverse,
                               :gen_fold_right].sample) }
  progs.join("\n")
end

# Deep nested define inside body (= triggers nested letrec desugaring)
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

# Recursive cons-builder using lambda with capture
def gen_recursive_cons_capture
  n = rand(50) + 10
  <<~SCM
    (define (build-n n)
      (define base #{rnum})
      (define (helper i)
        (if (= i n) '() (cons (+ i base) (helper (+ i 1)))))
      (helper 0))
    (define (sum lst) (if (null? lst) 0 (+ (car lst) (sum (cdr lst)))))
    (display (sum (build-n #{n}))) (newline)
  SCM
end

# set! on captured var from inner closure (= mutable capture)
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

# vector of closures
def gen_vector_closures
  n = rand(8) + 3
  <<~SCM
    (define v (make-vector #{n} 0))
    (define (init i)
      (if (= i #{n}) 'done
          (begin (vector-set! v i (lambda () (* i i))) (init (+ i 1)))))
    (init 0)
    (define (sum i acc)
      (if (= i #{n}) acc (sum (+ i 1) (+ acc ((vector-ref v i))))))
    (display (sum 0 0)) (newline)
  SCM
end

# nested let with inner lambda capturing both levels
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

# letrec with non-lambda RHS (= uninitialized at use? Tests order)
def gen_letrec_value
  <<~SCM
    (display
      (letrec ((x 10) (y (lambda () x)))
        (y)))
    (newline)
  SCM
end

# Heavily nested let inside lambda inside let
def gen_deep_nest
  <<~SCM
    (display
      (let ((a 1))
        (let ((b 2))
          (let ((f (lambda (x)
                     (let ((y (+ x a)))
                       (let ((z (+ y b)))
                         (* z z))))))
            (f 3)))))
    (newline)
  SCM
end

# Quoted nested data structure
def gen_deep_quote
  <<~SCM
    (define d '((1 2) (3 (4 5)) (6 7 (8 (9 10)))))
    (define (flat-sum lst)
      (cond ((null? lst) 0)
            ((pair? lst) (+ (flat-sum (car lst)) (flat-sum (cdr lst))))
            ((number? lst) lst)
            (else 0)))
    (display (flat-sum d)) (newline)
  SCM
end

GENERATORS = [
  :gen_mutual_internal,
  :gen_capturing_filter,
  :gen_callcc_deep,
  :gen_named_let,
  :gen_letrec_mutual,
  :gen_y_factorial,
  :gen_cons_walk,
  :gen_dotted_rest,
  :gen_map_capturing,
  :gen_quote_data,
  :gen_vector,
  :gen_string,
  :gen_do_loop,
  :gen_case,
  :gen_multi_internal,
  :gen_let_shadowing,
  :gen_closure_chain,
  :gen_apply,
  :gen_callcc_escape,
  :gen_reverse,
  :gen_fold_right,
  :gen_combo,
  :gen_nested_define,
  :gen_recursive_cons_capture,
  :gen_mutable_capture,
  :gen_vector_closures,
  :gen_nested_let_capture,
  :gen_letrec_value,
  :gen_deep_nest,
  :gen_deep_quote,
]

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

def run_program(bin, prog, env = {})
  # Run by writing prog to a tmp file (chez --script needs a path; precise
  # supports stdin via `-`, but a tmp file works for both).
  require 'tempfile'
  Tempfile.create(['fuzz', '.scm']) do |f|
    f.write(prog); f.flush
    args = if bin == ORACLE
             ['timeout', TIME.to_s, bin, *ORACLE_ARGS, f.path]
           else
             ['timeout', TIME.to_s, bin, '-q', f.path]
           end
    return Open3.capture3(env, *args)
  end
end

def kind(status)
  return :timeout if status.exitstatus == 124
  return :segv    if status.signaled? || status.exitstatus == 139
  return :error   unless status.success?
  :ok
end

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

puts "fuzz: N=#{N} GC_BACKEND=#{GC_BACKEND} seed=#{SEED}"
fails = 0
pass = 0
N.times do |i|
  gen = GENERATORS.sample
  prog = send(gen)

  # Oracle: libgc ascheme
  o_out, _o_err, o_status = run_program(ORACLE, prog)
  o_kind = kind(o_status)
  STDERR.puts "DBG[#{i}] gen=#{gen} oracle_kind=#{o_kind} oracle_st=#{o_status}" if ENV['FUZZ_DEBUG']

  if o_kind != :ok
    # Oracle itself failed → skip (probably exercises something libgc doesn't support)
    next
  end

  # Subject under test: precise + stress
  p_out, p_err, p_status = run_program(
    PRECISE, prog,
    'BARUBY_GC_STRESS' => '1'
  )
  p_kind = kind(p_status)

  # Filter stress diagnostic lines
  p_out_clean = p_out.lines.reject { |l| l.start_with?('[baruby_gc') || l.start_with?('[aot') }.join

  if p_kind == :segv || p_kind == :timeout || (p_kind == :ok && p_out_clean != o_out)
    fails += 1
    path = File.join(FAILDIR, format("%03d_%s.scm", fails, gen))
    File.write(path, prog)
    summary = "FAIL ##{i+1} #{gen}: precise=#{p_kind} oracle=#{o_kind} → #{path}"
    if p_kind == :ok && p_out_clean != o_out
      summary += " (output mismatch: expected #{o_out.inspect}, got #{p_out_clean.inspect})"
    end
    puts summary
    if fails >= 10
      puts "fuzz: too many failures, stopping"
      break
    end
  else
    pass += 1
    print "." if (i % 10) == 9
    $stdout.flush if (i % 10) == 9
  end
end
puts
puts "fuzz: pass=#{pass} fail=#{fails}"
exit(fails > 0 ? 1 : 0)
