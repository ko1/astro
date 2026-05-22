#!/usr/bin/env ruby
# Adapt chibi-scheme's tests/r5rs-tests.scm to ascheme.
#
# chibi defines `test` and `test-assert` as syntax-rules macros and uses
# them inline.  ascheme doesn't implement syntax-rules, so we:
#   1. Strip the (define-syntax test ...) and (define-syntax test-assert ...)
#      blocks at the top of the file.
#   2. Replace `(test ...)` and `(test-assert ...)` call forms with plain
#      procedure calls — `test-proc` and `test-assert-proc` — defined in
#      our prelude.
#   3. Skip whole top-level forms that use `define-syntax`, `let-syntax`,
#      `letrec-syntax`, or invoke user-defined macros — ascheme can't
#      evaluate them anyway.
#
# The result is written to stdout.  Run with:
#   ruby test/r5rs_adapt.rb < chibi/r5rs-tests.scm > test/r5rs_chibi.scm

require 'strscan'

src = ARGF.read

# Walk the source as a sequence of top-level s-expressions.  For each form,
# decide whether to keep, drop, or transform it.
def each_form(src)
  i = 0
  while i < src.length
    # skip whitespace + comments
    while i < src.length && src[i] =~ /\s/
      i += 1
    end
    if i < src.length && src[i] == ';'
      while i < src.length && src[i] != "\n"; i += 1; end
      next
    end
    break if i >= src.length

    if src[i] != '('
      # not a list form — pull it as a single token
      start = i
      while i < src.length && src[i] !~ /\s/
        i += 1
      end
      yield src[start...i], false
      next
    end

    # balanced sexp
    start = i
    depth = 0
    in_string = false
    in_char = false
    while i < src.length
      c = src[i]
      if in_string
        if c == '\\' then i += 2; next
        elsif c == '"' then in_string = false
        end
      elsif in_char
        # #\<char> — single token; just advance one
        in_char = false
      elsif c == '"' then in_string = true
      elsif c == '#' && src[i+1] == "\\"
        in_char = true
        i += 1   # skip the #, will skip the \ on next iter
      elsif c == ';'
        while i < src.length && src[i] != "\n"; i += 1; end
        next
      elsif c == '('
        depth += 1
      elsif c == ')'
        depth -= 1
        if depth == 0
          i += 1
          break
        end
      end
      i += 1
    end
    form = src[start...i]
    yield form, true
  end
end

# Determine whether a form references syntax-rules, define-syntax,
# let-syntax, letrec-syntax — anything we'd need a real macro system to
# handle.  Tests that introduce their own macros are skipped wholesale.
def needs_macros?(form)
  form =~ /\b(?:syntax-rules|define-syntax|let-syntax|letrec-syntax)\b/
end

# Top-level (define-syntax test ...) and (define-syntax test-assert ...)
# blocks are stripped; we provide our own test-proc / test-assert-proc in
# the prelude.
def is_test_macro_def?(form)
  form =~ /\A\(\s*define-syntax\s+(?:test|test-assert)\b/
end

# Transform `(test ...)` and `(test-assert ...)` *call* forms into
# `(test-proc ...)` / `(test-assert-proc ...)`.  We only touch the
# leading head — the args (which can be arbitrary scheme) pass through.
def transform_calls(form)
  form
    .sub(/\A\(\s*test-assert\s+/, '(test-assert-proc ')
    .sub(/\A\(\s*test\s+/, '(test-proc ')
end

prelude = <<~SCM
  ;; Adapted from chibi-scheme tests/r5rs-tests.scm.  See
  ;; test/r5rs_adapt.rb for the transformation we apply.
  (define *tests-run* 0)
  (define *tests-passed* 0)
  (define (test-proc . args)
    ;; (test-proc expected actual)  or  (test-proc name expected actual)
    (set! *tests-run* (+ *tests-run* 1))
    (let ((expected (if (= (length args) 2) (car args) (cadr args)))
          (actual   (if (= (length args) 2) (cadr args) (caddr args))))
      (cond
       ((equal? actual expected)
        (set! *tests-passed* (+ *tests-passed* 1)))
       (else
        (display "FAIL #") (display *tests-run*)
        (display " expected=") (write expected)
        (display " actual=") (write actual) (newline)))))
  (define (test-assert-proc x) (test-proc #t x))
  (define (test-begin . n) #f)
  (define (test-end)
    (display *tests-passed*) (display " / ") (display *tests-run*)
    (display " r5rs tests passed") (newline))
  (define (flush-output) #f)               ;; noop — chibi's test driver flushes stdout
  (define call-with-output-string
    (lambda (proc)
      (let ((s "<dummy>")) (proc 'dummy-port) s))) ;; just stub for the test driver
SCM

puts prelude

each_form(src) do |form, listp|
  next unless listp
  next if is_test_macro_def?(form)
  next if needs_macros?(form)
  puts transform_calls(form)
  puts
end
