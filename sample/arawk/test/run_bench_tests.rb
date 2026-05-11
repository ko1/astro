#!/usr/bin/env ruby
# arawk vs gawk: runs the goawk bench scripts (testdata/tt.*) on a
# canonical input file and verifies arawk's output matches gawk's.
# Tests known to require features arawk doesn't yet implement are
# explicitly skipped with a reason; everything else must match
# byte-for-byte.

require 'open3'

ROOT     = File.expand_path('..', __dir__)
BIN      = File.join(ROOT, 'arawk')
GOAWKDIR = File.join(ROOT, 'goawk')
TESTDATA = File.join(GOAWKDIR, 'testdata')
INPUT    = File.join(TESTDATA, 'foo.td')

unless File.executable?(BIN)
  abort "arawk not built (run `make`)"
end
unless File.directory?(GOAWKDIR) && File.exist?(INPUT)
  abort "goawk submodule missing — run `git submodule update --init sample/arawk/goawk`"
end
unless system('which gawk >/dev/null 2>&1')
  abort 'gawk not in PATH (apt install gawk)'
end

# Feature-skip table — keep entries alphabetic so additions are easy
# to spot.  When arawk gains a feature, remove the entry here.
SKIP = {
  'tt.08z_regex_simple'      => 'regex /.../ pattern',
  'tt.09_regex_starts_with'  => 'regex !/.../ pattern',
  'tt.10_regex_ends_with'    => 'regex /.../ pattern',
  'tt.10a_regex_ends_with_var' => 'regex $0 ~ var',
  'tt.15_format_lines'       => 'regex `~` + sub/gsub',
  'tt.big_complex_program'   => 'regex (the rest works)',
}

tests = Dir.glob("#{TESTDATA}/tt.*").sort
pass  = fail = skipped = 0
failures = []

tests.each do |path|
  name = File.basename(path)
  if SKIP.key?(name)
    skipped += 1
    puts format('  SKIP %-30s (%s)', name, SKIP[name])
    next
  end

  want, _, st_gawk = Open3.capture3('gawk', '-f', path, INPUT)
  got,  err, st_arawk = Open3.capture3(BIN, '--plain', '-f', path, INPUT)
  if st_gawk.exitstatus != 0
    skipped += 1
    puts format('  SKIP %-30s (gawk error)', name)
    next
  end

  if got == want && st_arawk.exitstatus == 0
    pass += 1
    puts format('  ok   %-30s', name)
  else
    fail += 1
    puts format('  NG   %-30s', name)
    failures << [name, want, got, err, st_arawk.exitstatus]
  end
end

if fail > 0
  puts
  puts '=== failures ==='
  failures.each do |name, want, got, err, ec|
    puts "--- #{name} (arawk exit=#{ec}) ---"
    if err && !err.empty?
      puts "stderr: #{err[0, 400]}"
    end
    if got.lines.length > 5 || want.lines.length > 5
      puts "diff (first 3 lines):"
      require 'tempfile'
      Tempfile.create('want') do |fa|
        Tempfile.create('got') do |fb|
          fa.write(want); fa.flush
          fb.write(got);  fb.flush
          puts `diff #{fa.path} #{fb.path} | head -6`
        end
      end
    else
      puts "want: #{want.inspect}"
      puts "got:  #{got.inspect}"
    end
  end
end

puts
puts "#{pass}/#{pass + fail} pass, #{skipped} skipped (#{SKIP.size} features pending)"
exit(fail == 0 ? 0 : 1)
