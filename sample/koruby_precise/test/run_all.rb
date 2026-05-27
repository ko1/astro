# Run all koruby tests via require_relative.  Each suite reports its own
# pass/fail; we aggregate.
DIR = File.dirname(__FILE__) rescue "."
SUITES = %w[
  test_integer test_array test_hash test_string test_block
  test_class test_control test_proc test_exception test_cref
]
total_pass = 0
total_fail = 0
SUITES.each do |s|
  path = "#{DIR}/#{s}.rb"
  next unless File.exist?(path)
  # Run as a child process so each suite has fresh globals.
  # (No fork/system available, so use load — but TESTS array would clash.
  # Workaround: each suite calls exit at end with a code we read via $?.)
  # For now, just print which suite we'd run — they should be invoked
  # individually from the Makefile.
  puts "** #{s}"
  load path
end
