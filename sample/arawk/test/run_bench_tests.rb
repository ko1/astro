#!/usr/bin/env ruby
# arawk vs gawk: runs the goawk bench scripts (testdata/tt.*) on a
# canonical input file and verifies arawk's output matches gawk's.
# Tests known to require features arawk doesn't yet implement are
# explicitly skipped with a reason; everything else must match
# byte-for-byte.

require 'open3'
require 'fileutils'

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

# arawk runs in both `--plain` (no AOT) and AOT-baked mode.  AOT
# regressions show up when SD-specialised dispatchers produce different
# output from the plain interpreter — exactly the surface we care about.
MODES = [
  ['plain', ['--plain']],
  ['aot',   ['-c', '--ccs']],
]

tests = Dir.glob("#{TESTDATA}/tt.*").sort
pass  = fail = skipped = 0
failures = []

start = Time.now

tests.each do |path|
  name = File.basename(path)
  if SKIP.key?(name)
    skipped += 1
    puts format('  SKIP %-30s (%s)', name, SKIP[name])
    next
  end

  want, _, st_gawk = Open3.capture3('gawk', '-f', path, INPUT)
  if st_gawk.exitstatus != 0
    skipped += 1
    puts format('  SKIP %-30s (gawk error)', name)
    next
  end

  outcomes = MODES.map do |mode, flags|
    # `-c` writes a per-mode code_store; isolate them so plain runs
    # don't pick up an AOT artefact and vice-versa.
    Dir.chdir(ROOT) do
      got, err, st = Open3.capture3(BIN, *flags, '-f', path, INPUT)
      [mode, got, err, st.exitstatus]
    end
  end

  bad = outcomes.reject { |mode, got, err, ec| got == want && ec == 0 }
  if bad.empty?
    pass += 1
    puts format('  ok   %-30s', name)
  else
    fail += 1
    puts format('  NG   %-30s  (failed in: %s)', name, bad.map(&:first).join(', '))
    failures << [name, want, outcomes]
  end
end

elapsed = Time.now - start

if fail > 0
  puts
  puts '=== failures ==='
  require 'tempfile'
  failures.each do |name, want, outcomes|
    outcomes.each do |mode, got, err, ec|
      next if got == want && ec == 0
      puts "--- #{name} (#{mode}, exit=#{ec}) ---"
      puts "stderr: #{err[0, 400]}" if err && !err.empty?
      if got.lines.length > 5 || want.lines.length > 5
        Tempfile.create('want') do |fa|
          Tempfile.create('got') do |fb|
            fa.write(want); fa.flush
            fb.write(got);  fb.flush
            puts "diff (first 4 lines):"
            puts `diff #{fa.path} #{fb.path} | head -4`
          end
        end
      else
        puts "want: #{want.inspect}"
        puts "got:  #{got.inspect}"
      end
    end
  end
end

puts
puts "#{pass}/#{pass + fail} pass (#{MODES.size} modes each), #{skipped} skipped (#{SKIP.size} features pending)  (#{'%.2f' % elapsed}s)"
exit(fail == 0 ? 0 : 1)
