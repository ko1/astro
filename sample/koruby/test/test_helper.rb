# Minimal test framework.  Each test_xxx top-level method is collected
# explicitly in TESTS and run.  Output: "OK <suite>" or per-failure detail.

$pass = 0
$fail = 0
$current = nil

def assert(cond, msg = "assertion failed")
  if cond
    $pass += 1
  else
    $fail += 1
    puts "FAIL #{$current}: #{msg}"
  end
end

def assert_equal(expected, actual, msg = nil)
  assert(expected == actual, msg || "expected #{expected.inspect}, got #{actual.inspect}")
end

def assert_raise(_klass = nil)
  raised = false
  begin
    yield
  rescue => _e
    raised = true
  end
  assert(raised, "expected an exception, got none")
end

def run_test(name)
  $current = name
  send(name)
end

def report(suite)
  if $fail == 0
    puts "OK #{suite} (#{$pass})"
    exit 0
  else
    puts "FAIL #{suite}: #{$fail}/#{$pass + $fail}"
    exit 1
  end
end
