#!/usr/bin/env ruby
# Test harness for anlox (Lox on ASTro).
#
# Uses Crafting Interpreters' canonical self-contained format: each `.lox`
# fixture annotates its own expected output with comments, so no external
# reference interpreter is required.
#
#   print 1 + 2;                      // expect: 3
#   foo;                              // expect runtime error: Undefined variable 'foo'.
#   var a =                           // Error at end: Expect expression.   (compile error)
#
# Markers (matching the book's test suite):
#   // expect: TEXT                 — the next stdout line must equal TEXT
#   // expect runtime error: MSG    — program exits 70 and MSG appears on stderr
#   // [compile] / // Error...      — program exits 65 (syntax/resolve error)
#
# Optionally, set ANLOX_REF=/path/to/jlox to ALSO run each well-formed fixture
# through a reference Lox and diff stdout (true differential testing).
require 'open3'

HERE = File.dirname(File.expand_path(__FILE__))
ANLOX = ENV['ANLOX'] || File.join(HERE, '..', 'anlox')
REF   = ENV['ANLOX_REF']   # optional reference interpreter

$pass = 0
$fail = 0

def run(prog, file)
  Open3.capture3(prog, file)   # [out, err, status]
end

def check(path)
  src = File.read(path)
  expects = []
  rt_error = nil
  compile_error = false
  src.each_line do |ln|
    if (m = ln.match(%r{//\s*expect:\s?(.*)$}))            then expects << m[1].rstrip
    elsif (m = ln.match(%r{//\s*expect runtime error:\s?(.*)$})) then rt_error = m[1].rstrip
    elsif ln.match(%r{//\s*\[compile\]}) || ln.match(%r{//\s*Error}) then compile_error = true
    end
  end

  out, err, st = run(ANLOX, path)
  name = File.basename(path)
  got = out.split("\n", -1); got.pop if got.last == ""

  ok = true; why = nil
  if compile_error
    ok = (st.exitstatus == 65); why = "expected compile error (exit 65), got exit #{st.exitstatus}" unless ok
  elsif rt_error
    if st.exitstatus != 70 then ok = false; why = "expected runtime error (exit 70), got #{st.exitstatus}"
    elsif !err.include?(rt_error) then ok = false; why = "stderr missing #{rt_error.inspect}: got #{err.inspect}"
    elsif got != expects then ok = false; why = "stdout #{got.inspect} != #{expects.inspect}"
    end
  else
    if st.exitstatus != 0 then ok = false; why = "exit #{st.exitstatus}, stderr=#{err.inspect}"
    elsif got != expects then ok = false; why = "stdout #{got.inspect} != expected #{expects.inspect}"
    end
  end

  # optional differential check against a reference interpreter
  if ok && REF && !compile_error && !rt_error
    rout, _rerr, _rst = run(REF, path)
    rgot = rout.split("\n", -1); rgot.pop if rgot.last == ""
    if rgot != got then ok = false; why = "ref mismatch: anlox #{got.inspect} vs ref #{rgot.inspect}" end
  end

  if ok then $pass += 1
  else $fail += 1; puts "FAIL #{name}: #{why}"
  end
end

Dir[File.join(HERE, 'cases', '*.lox')].sort.each { |p| check(p) }

puts
puts "anlox tests: #{$pass} passed, #{$fail} failed"
exit($fail == 0 ? 0 : 1)
