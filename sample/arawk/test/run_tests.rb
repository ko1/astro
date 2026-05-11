#!/usr/bin/env ruby
# arawk Phase 0+1 smoke test runner.  Drives the `arawk` binary via
# popen3 and compares stdout against expected output.

require 'open3'

BIN = File.expand_path('../arawk', __dir__)
unless File.executable?(BIN)
  abort "arawk not built (run `make`)"
end

CASES = [
  # [name, awk-program, stdin (nil = none), expected-stdout, expected-rc]
  ['begin-arith',     'BEGIN { print 1 + 2 * 3 }',                   nil,                     "7\n",                          0],
  ['begin-string',    'BEGIN { print "hello" }',                     nil,                     "hello\n",                       0],
  ['concat',          'BEGIN { print "a" "b", "x" "y", 1+2 }',       nil,                     "ab xy 3\n",                     0],
  ['exit-code',       'BEGIN { exit 42 }',                           nil,                     "",                              42],
  ['main-print-dollar', '{ print $1, NR }',                          "a b c\nd e f\n",        "a 1\nd 2\n",                    0],
  ['print-no-args',   '{ print }',                                   "foo\nbar\n",            "foo\nbar\n",                    0],
  ['begin-main-end',  'BEGIN { print "s" } { print $1 } END { print "e", NR }',
                                                                     "x\ny\n",                "s\nx\ny\ne 2\n",                0],
  ['nf-count',        '{ wc = wc + NF } END { print wc }',           "a b c\nd e\nf\n",       "6\n",                           0],
  ['while-sum',       'BEGIN { i = 1; s = 0; while (i <= 10) { s = s + i; i = i + 1 }; print s }',
                                                                     nil,                     "55\n",                          0],
  ['if-else',         'BEGIN { if (3 > 2) print "yes"; else print "no" }', nil,               "yes\n",                         0],
  ['next-skip',       'NR == 2 { next } { print }',                  "a\nb\nc\n",             "a\nc\n",                        0],
  ['pattern-eq',      '$1 == 2 { print "found:", $2 }',              "1 alpha\n2 beta\n3 g\n", "found: beta\n",                0],
  ['default-action',  'NR == 2',                                     "x\ny\nz\n",             "y\n",                           0],
  ['mod-arith',       'BEGIN { print 17 % 5, 100 / 4, 2^10 }',       nil,                     "2 25 1024\n",                   0],
  ['concat-vs-add',   'BEGIN { x = 1; print x + 1, x "1" }',         nil,                     "2 11\n",                        0],
  ['fs-default-ws',   '{ print $2 }',                                "  a  b  c\n",           "b\n",                           0],
  ['nf-read',         '{ print NF }',                                "a b c\nd\n\n",          "3\n1\n0\n",                     0],
  ['line-comment',    "BEGIN {\n# this is a comment\nprint 42 }",    nil,                     "42\n",                          0],
]

pass = fail = 0
failures = []

CASES.each do |name, prog, stdin, want_out, want_rc|
  out, err, status = Open3.capture3(BIN, '--plain', prog, stdin_data: stdin || '')
  got_rc = status.exitstatus
  if out == want_out && got_rc == want_rc
    pass += 1
    puts "  ok  #{name}"
  else
    fail += 1
    failures << [name, prog, want_out, out, want_rc, got_rc, err]
    puts "  NG  #{name}"
  end
end

if fail > 0
  puts
  puts "=== failures ==="
  failures.each do |name, prog, want_out, got_out, want_rc, got_rc, err|
    puts "--- #{name} ---"
    puts "  prog: #{prog.inspect}"
    puts "  want: rc=#{want_rc}  out=#{want_out.inspect}"
    puts "  got : rc=#{got_rc}   out=#{got_out.inspect}"
    puts "  err : #{err.inspect}" unless err.empty?
  end
end

puts
puts "#{pass}/#{pass + fail} pass"
exit(fail == 0 ? 0 : 1)
