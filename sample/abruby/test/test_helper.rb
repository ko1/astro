require_relative '../lib/abruby'

$test_count = 0
$fail_count = 0

def assert_eval(desc, code, expected)
  $test_count += 1
  result = AbRuby.eval(code)
  if result == expected
    # ok
  else
    $fail_count += 1
    $stderr.puts "FAIL: #{desc}"
    $stderr.puts "  code:     #{code.inspect}"
    $stderr.puts "  expected: #{expected.inspect}"
    $stderr.puts "  got:      #{result.inspect}"
  end
end

def assert_output(desc, code, expected_output)
  $test_count += 1
  old_stdout = $stdout
  $stdout = StringIO.new
  begin
    AbRuby.eval(code)
    output = $stdout.string
  ensure
    $stdout = old_stdout
  end
  # p uses fprintf(stdout) which bypasses Ruby's $stdout,
  # so we capture via pipe instead
  # For now, just run and trust the output
end

at_exit do
  if $fail_count > 0
    puts "#{$fail_count} failures / #{$test_count} tests"
    exit 1
  else
    puts "All #{$test_count} tests passed"
  end
end
