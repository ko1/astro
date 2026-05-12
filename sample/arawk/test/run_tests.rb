#!/usr/bin/env ruby
# arawk smoke tests — one focused case per language feature, with
# boundary cases (uninit, empty input, large values, fp coercion).
# Each case runs arawk in both `--plain` (no AOT) and `-c` AOT modes
# to catch regressions specific to the SD-bake path.

require 'open3'
require 'fileutils'
require 'tmpdir'

BIN = File.expand_path('../arawk', __dir__)
abort "arawk not built (run `make`)" unless File.executable?(BIN)

# A case is [name, awk-program, stdin (or nil), expected-stdout, expected-rc].
CASES = [
  # --- Phase 0+1: basics ----------------------------------------------------
  ['begin-arith',        'BEGIN { print 1 + 2 * 3 }',                            nil,                "7\n",                          0],
  ['begin-string',       'BEGIN { print "hello" }',                              nil,                "hello\n",                       0],
  ['concat',             'BEGIN { print "a" "b", "x" "y", 1+2 }',                nil,                "ab xy 3\n",                     0],
  ['exit-code',          'BEGIN { exit 42 }',                                    nil,                "",                              42],
  ['main-print-dollar',  '{ print $1, NR }',                                     "a b c\nd e f\n",   "a 1\nd 2\n",                    0],
  ['print-no-args',      '{ print }',                                            "foo\nbar\n",       "foo\nbar\n",                    0],
  ['begin-main-end',     'BEGIN { print "s" } { print $1 } END { print "e", NR }', "x\ny\n",          "s\nx\ny\ne 2\n",                0],
  ['nf-count',           '{ wc = wc + NF } END { print wc }',                    "a b c\nd e\nf\n",  "6\n",                           0],
  ['while-sum',          'BEGIN { i = 1; s = 0; while (i <= 10) { s = s + i; i = i + 1 }; print s }', nil, "55\n",                    0],
  ['if-else',            'BEGIN { if (3 > 2) print "yes"; else print "no" }',    nil,                "yes\n",                         0],
  ['next-skip',          'NR == 2 { next } { print }',                           "a\nb\nc\n",        "a\nc\n",                        0],
  ['pattern-eq',         '$1 == 2 { print "found:", $2 }',                       "1 alpha\n2 beta\n3 g\n", "found: beta\n",          0],
  ['default-action',     'NR == 2',                                              "x\ny\nz\n",        "y\n",                           0],
  ['mod-arith',          'BEGIN { print 17 % 5, 100 / 4, 2^10 }',                nil,                "2 25 1024\n",                   0],
  ['concat-vs-add',      'BEGIN { x = 1; print x + 1, x "1" }',                  nil,                "2 11\n",                        0],
  ['fs-default-ws',      '{ print $2 }',                                         "  a  b  c\n",      "b\n",                           0],
  ['nf-read',            '{ print NF }',                                         "a b c\nd\n\n",     "3\n1\n0\n",                     0],
  ['comment-stripping',  "BEGIN {\n# comment\nprint 42 }",                       nil,                "42\n",                          0],
  ['neg-and-paren',      'BEGIN { print -3 + (4 - 2) * -1 }',                    nil,                "-5\n",                          0],
  ['boolean-shortcircuit', 'BEGIN { print (0 && 1/0), (1 || 1/0) }',             nil,                "0 1\n",                         0],

  # --- Phase 1.5: for / ++ -- / compound assign / length -------------------
  ['for-c-style',        'BEGIN { for (i = 1; i <= 5; i = i + 1) printf "%d ", i; print "" }', nil, "1 2 3 4 5 \n",                  0],
  ['for-postinc',        'BEGIN { s = 0; for (i = 1; i <= 10; i++) s += i; print s }',          nil, "55\n",                         0],
  ['for-postdec',        'BEGIN { s = 0; for (i = 10; i > 0; i--) s += i; print s }',           nil, "55\n",                         0],
  ['for-empty',          'BEGIN { i = 0; for (;;) { if (i >= 3) break; i++ } print i }',        nil, "3\n",                          0],
  ['for-nested',         'BEGIN { for (i=1; i<=3; i++) for (j=1; j<=3; j++) s += i*j; print s }', nil, "36\n",                       0],
  ['preinc',             'BEGIN { i = 5; print ++i, i }',                          nil,                "6 6\n",                        0],
  ['predec',             'BEGIN { i = 5; print --i, i }',                          nil,                "4 4\n",                        0],
  ['postinc-rvalue',     'BEGIN { i = 5; print i++, i }',                          nil,                "5 6\n",                        0],
  ['compound-add',       'BEGIN { x = 10; x += 5; print x }',                      nil,                "15\n",                         0],
  ['compound-sub-mul-div', 'BEGIN { x = 100; x -= 30; x *= 2; x /= 5; print x }',  nil,                "28\n",                         0],
  ['compound-mod',       'BEGIN { x = 100; x %= 7; print x }',                     nil,                "2\n",                          0],
  ['compound-pow',       'BEGIN { x = 2; x ^= 8; print x }',                       nil,                "256\n",                        0],
  ['length-noparen',     '{ print length }',                                       "hello\nbyebye\n", "5\n6\n",                       0],
  ['length-paren-empty', 'BEGIN { print length() + 1 }',                            "",                "1\n",                          0],
  ['length-arg-string',  'BEGIN { print length("hello world") }',                   nil,                "11\n",                         0],

  # --- Phase 1.6: arrays / ternary / in / for-in / delete ------------------
  ['array-basic',        'BEGIN { a["foo"] = 1; a["bar"] = 2; print a["foo"], a["bar"] }', nil, "1 2\n",                              0],
  ['array-numeric-key',  'BEGIN { for (i=1; i<=3; i++) a[i] = i*i; print a[1], a[2], a[3] }', nil, "1 4 9\n",                          0],
  ['array-compound',     '{ counts[$1]++ } END { print counts["a"], counts["b"] }', "a\na\nb\na\nb\n", "3 2\n",                       0],
  ['ternary-basic',      'BEGIN { print (3 > 2 ? "yes" : "no") }',                nil,                "yes\n",                        0],
  ['ternary-nested',     'BEGIN { x = 7; print (x < 5 ? "lo" : x < 10 ? "mid" : "hi") }', nil,        "mid\n",                        0],
  ['in-operator',        'BEGIN { a["k"] = 1; print ("k" in a), ("z" in a) }',    nil,                "1 0\n",                        0],
  ['for-in',             'BEGIN { a[1]=10; a[2]=20; a[3]=30; for (k in a) s += a[k]; print s }', nil, "60\n",                         0],
  ['delete-elem',        'BEGIN { a[1]=10; a[2]=20; delete a[1]; print (1 in a), (2 in a) }', nil,    "0 1\n",                        0],
  ['delete-all',         'BEGIN { a[1]=10; a[2]=20; delete a; print (1 in a), (2 in a) }', nil,       "0 0\n",                        0],
  ['multidim-key',       'BEGIN { a[1, 2] = 99; print a[1, 2], (a[1,2] == a[1 SUBSEP 2]) }', nil,     "99 1\n",                       0],

  # --- Phase 1.7: printf / sprintf / builtins ------------------------------
  ['printf-int-str',     'BEGIN { printf "%d %s\n", 7, "hi" }',                   nil,                "7 hi\n",                       0],
  ['printf-width-prec',  'BEGIN { printf "%5d|%-5d|%.3f\n", 42, 42, 3.14159 }',   nil,                "   42|42   |3.142\n",          0],
  ['printf-percent',     'BEGIN { printf "%d%% done\n", 50 }',                    nil,                "50% done\n",                    0],
  ['printf-star',        'BEGIN { printf "%*d\n", 5, 7 }',                        nil,                "    7\n",                       0],
  ['sprintf-basic',      'BEGIN { s = sprintf("[%05d]", 42); print s }',          nil,                "[00042]\n",                     0],
  ['substr-2arg',        'BEGIN { print substr("abcdef", 3) }',                   nil,                "cdef\n",                        0],
  ['substr-3arg',        'BEGIN { print substr("abcdef", 2, 3) }',                nil,                "bcd\n",                         0],
  ['index-found',        'BEGIN { print index("hello", "ll") }',                  nil,                "3\n",                           0],
  ['index-not-found',    'BEGIN { print index("hello", "xy") }',                  nil,                "0\n",                           0],
  ['split-default',      'BEGIN { n = split("a b c", parts); print n, parts[1], parts[3] }', nil,     "3 a c\n",                       0],
  ['split-sep',          'BEGIN { n = split("a:b:c", parts, ":"); print n, parts[2] }', nil,           "3 b\n",                         0],
  ['tolower-toupper',    'BEGIN { print tolower("AbC"), toupper("AbC") }',        nil,                "abc ABC\n",                     0],
  ['int-trunc',          'BEGIN { print int(3.7), int(-3.7) }',                   nil,                "3 -3\n",                        0],
  ['sqrt-builtin',       'BEGIN { print sqrt(16), sqrt(2) }',                     nil,                "4 1.41421\n",                   0],

  # --- $N assignment / inc / dec -------------------------------------------
  ['field-assign-const', '{ $2 = "X"; print }',                                   "a b c\nd e f\n",   "a X c\nd X f\n",                0],
  ['field-postdec',      '{ $1--; print $1 }',                                    "10 x\n20 y\n",     "9\n19\n",                       0],

  # --- Phase 1.8: user-defined functions -----------------------------------
  ['func-basic',         'function f(x) { return x + 1 } BEGIN { print f(10) }',  nil,                "11\n",                          0],
  ['func-recurse',       'function fib(n) { if (n < 2) return n; return fib(n-1) + fib(n-2) } BEGIN { print fib(10) }', nil, "55\n", 0],
  ['func-multi-arg',     'function sum3(a, b, c) { return a + b + c } BEGIN { print sum3(1, 2, 3) }', nil, "6\n",                     0],
  ['func-local-extra',   'function f(a,    i) { for (i=0; i<a; i++) s += i; return s } BEGIN { print f(5) }', nil, "10\n",            0],
  ['func-touches-global', 'function bump() { g++ } BEGIN { g = 10; bump(); bump(); print g }', nil,    "12\n",                         0],
  ['func-no-return',     'function noret(x) { x = x * 2 } BEGIN { print noret(5) "x" }',         nil, "x\n",                          0],

  # --- Phase 1.9: pipe / redirect -----------------------------------------
  # (file redirect tests use $TMPDIR-allocated paths; pipe goes to sort.)
  # All three go through one sort pipe — output is deterministic.
  ['pipe-to-sort',       'BEGIN { print "c" | "sort"; print "a" | "sort"; print "b" | "sort" }', nil, "a\nb\nc\n",                  0],

  # --- Phase 1.10: printf redirect ----------------------------------------
  ['printf-pipe-sort',   'BEGIN { printf "%d\n", 30 | "sort"; printf "%d\n", 10 | "sort"; printf "%d\n", 20 | "sort" }', nil, "10\n20\n30\n", 0],

  # --- Phase 1.11: close / fflush / system --------------------------------
  ['close-pipe',         'BEGIN { print "x" | "cat"; close("cat"); print "y" | "cat" }', nil, "x\ny\n",                              0],
  ['fflush-noop',        'BEGIN { print "a"; fflush() }',                                nil, "a\n",                                 0],
  ['system-echo',        'BEGIN { print "before"; system("echo mid"); print "after" }',  nil, "before\nmid\nafter\n",                0],

  # --- Phase 1.12: getline ------------------------------------------------
  ['getline-cmd-var',    'BEGIN { ("echo hi") | getline x; print x }',                   nil, "hi\n",                                0],
  ['getline-cmd',        'BEGIN { ("echo hi") | getline; print $0 }',                    nil, "hi\n",                                0],
  ['getline-cur',        'BEGIN { while ((getline) > 0) print "line:", $0 }',            "a\nb\n", "line: a\nline: b\n",            0],
  ['getline-cur-var',    'BEGIN { while ((getline x) > 0) print "got:", x }',            "a\nb\n", "got: a\ngot: b\n",              0],

  # --- Phase 1.13: ENVIRON / ARGC / ARGV ----------------------------------
  ['environ-read',       'BEGIN { print ENVIRON["ARAWK_TEST_VAR"] }',                    nil, "smoke-value\n",                       0, { env: { 'ARAWK_TEST_VAR' => 'smoke-value' } }],
  ['argv-basic',         'BEGIN { print ARGC, ARGV[0] }',                                nil, "1 arawk\n",                            0],

  # --- Phase 1.15b: CONVFMT / FS rebind / NF= -----------------------------
  ['convfmt-effect',     'BEGIN { CONVFMT = "%.2f"; print 1/3 "" }',                     nil, "0.33\n",                              0],
  ['fs-rebind',          'BEGIN { FS = "," } { print $2 }',                              "a,b,c\nd,e,f\n", "b\ne\n",                 0],
  ['nf-grow',            '{ NF = 5; print }',                                            "a b c\n", "a b c  \n",                    0],
  ['nf-shrink',          '{ NF = 2; print }',                                            "a b c d\n", "a b\n",                       0],
  ['nf-after-field-assign', '{ $5 = "x"; print NF, $0 }',                                "a b\n", "5 a b   x\n",                     0],
  # `(i, j) in a` is a gawk extension; in POSIX you check membership by
  # the joined key directly.  We verify delete by reading post-delete value.
  ['multi-dim-delete',   'BEGIN { a[1,2] = 99; print a[1,2]; delete a[1,2]; print a[1,2] "_" }', nil, "99\n_\n",                     0],

  # --- do-while + nextfile ------------------------------------------------
  ['do-while-basic',     'BEGIN { i = 0; do { print i; i++ } while (i < 3) }',                 nil, "0\n1\n2\n",                       0],
  ['do-while-once',      'BEGIN { i = 100; do { print "ran"; i++ } while (i < 3) }',           nil, "ran\n",                           0],
  ['do-while-break',     'BEGIN { i = 0; do { if (i == 2) break; print i; i++ } while (1) }',  nil, "0\n1\n",                          0],
  ['do-while-continue',  'BEGIN { i = 0; do { i++; if (i % 2 == 0) continue; print i } while (i < 5) }', nil, "1\n3\n5\n",            0],
  ['do-while-return',    'function f() { do { return 42 } while (1) } BEGIN { print f() }',    nil, "42\n",                            0],

  # nextfile — uses :files to pre-create two input files.
  ['nextfile-basic',
    '$1 == "skip" { nextfile } { print FILENAME ":" $1 }',
    nil,
    "in0.txt:keep1\nin0.txt:keep2\nin1.txt:keep3\nin1.txt:keep4\n",
    0,
    { files: ["keep1\nkeep2\nskip\nshould_not_appear\n", "keep3\nkeep4\n"] },
  ],
  ['nextfile-first-line',
    'FNR == 1 { print "saw first of", FILENAME; nextfile }',
    nil,
    "saw first of in0.txt\nsaw first of in1.txt\n",
    0,
    { files: ["A1\nA2\nA3\n", "B1\nB2\n"] },
  ],
  ['nextfile-only-file',
    '$1 == "skip" { nextfile } { print $1 }',
    "first\nskip\nshould_not_appear\n",
    "first\n",
    0,
  ],

  # --- UTF-8 (LC_CTYPE auto-detected as UTF-8 by main.c) ----------------
  # The test environment runs in C.UTF-8 by default; these tests assert
  # codepoint-aware semantics matching gawk's default behaviour.
  ['utf8-length-1char',  'BEGIN { print length("あ") }',                        nil, "1\n",                       0],
  ['utf8-length-3char',  'BEGIN { print length("あいう") }',                    nil, "3\n",                       0],
  ['utf8-length-mixed',  'BEGIN { print length("café"), length("hello") }',     nil, "4 5\n",                     0],
  ['utf8-substr',        'BEGIN { print substr("あいうえお", 2, 2) }',           nil, "いう\n",                    0],
  ['utf8-substr-end',    'BEGIN { print substr("café", 3) }',                   nil, "fé\n",                      0],
  ['utf8-index',         'BEGIN { print index("xxあいうyy", "あい") }',         nil, "3\n",                       0],
  ['utf8-index-ascii',   'BEGIN { print index("xxxあい", "x") }',               nil, "1\n",                       0],
  ['utf8-index-miss',    'BEGIN { print index("hello", "あ") }',                nil, "0\n",                       0],

  # --- --byte mode overrides locale-driven UTF-8 -------------------------
  ['byte-length',        'BEGIN { print length("あ") }',                        nil, "3\n",                       0, { flags: ['--byte'] }],
  ['byte-substr',        'BEGIN { print length(substr("あいうえお", 4, 3)) }',  nil, "3\n",                       0, { flags: ['--byte'] }],

  # --- LC_ALL=C forces byte mode through main.c locale detection --------
  ['locale-c',           'BEGIN { print length("あ") }',                        nil, "3\n",                       0, { env: { 'LC_ALL' => 'C' } }],

  # --- Boundary / coercion tests ------------------------------------------
  ['uninit-arith',       'BEGIN { print x + 1, x "y" }',                          nil,                "1 y\n",                         0],
  ['empty-string-num',   'BEGIN { print "" + 5 }',                                nil,                "5\n",                           0],
  ['strnum-numeric',     '{ print $1 + 1 }',                                      "10\n20\n",         "11\n21\n",                      0],
  ['strnum-textual',     '{ print $1 + 1 }',                                      "foo\nbar\n",       "1\n1\n",                        0],
  ['strtod-no-inf-bug',  'BEGIN { print "informed" + 0, "infinity" + 0, "1e1000000" + 0 }', nil,      "0 0 inf\n",                     0],
  ['large-fixnum',       'BEGIN { print 1000000 * 1000000 }',                     nil,                "1000000000000\n",               0],
]

# Run each case in both interpreter modes (plain = no AOT, aot = -c).
# AOT path uses an isolated code_store directory so smoke runs don't
# stomp the user's local code_store.
MODES = {
  'plain' => ['--plain'],
  'aot'   => ['-c', '--ccs'],
}

start = Time.now
pass = fail = 0
failures = []

CASES.each do |row|
  name, prog, stdin, want_out, want_rc, opts = row
  opts ||= {}
  MODES.each do |mode, flags|
    Dir.mktmpdir('arawk-smoke-') do |dir|
      Dir.chdir(dir) do
        # opts[:files] = ["content1", "content2"] — write each as a
        # *relative* path so FILENAME shows the bare basename in
        # test expectations.  Useful for nextfile / FILENAME /
        # multi-file tests where a single stdin is not enough.
        file_args = []
        (opts[:files] || []).each_with_index do |content, i|
          path = "in#{i}.txt"
          File.write(path, content)
          file_args << path
        end
        # opts[:flags] = extra CLI flags appended after the mode flag
        # (e.g. ['--byte'] for UTF-8 byte-mode regression tests).
        argv = [BIN, *flags, *(opts[:flags] || []), prog, *file_args]
        env  = opts[:env] || {}
        got, err, status = Open3.capture3(env, *argv, stdin_data: stdin || '')
        got_rc = status.exitstatus
        if got == want_out && got_rc == want_rc
          pass += 1
        else
          fail += 1
          failures << [name, mode, prog, want_out, got, want_rc, got_rc, err]
        end
      end
    end
  end
end

elapsed = Time.now - start

failures.each do |name, mode, prog, want_out, got_out, want_rc, got_rc, err|
  puts "NG  #{name} (#{mode})"
  puts "  prog: #{prog.inspect}"
  puts "  want: rc=#{want_rc} out=#{want_out.inspect}"
  puts "  got : rc=#{got_rc}  out=#{got_out.inspect}"
  puts "  err : #{err.inspect}" unless err.empty?
end

n_cases = CASES.size
puts "#{pass}/#{pass + fail} pass across #{n_cases} cases × #{MODES.size} modes  (#{'%.2f' % elapsed}s)"
exit(fail == 0 ? 0 : 1)
