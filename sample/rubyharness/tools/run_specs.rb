#!/usr/bin/env ruby
# frozen_string_literal: true
#
# run_specs.rb — isolated per-file differential test driver for the koruby rebuild.
#
# Runs EACH test file in its OWN process.  A young interpreter SEGVs and hangs
# constantly; one crash must never abort the sweep.  Runs on CRuby: the
# interpreter under test cannot be trusted to drive itself.
#
# Two modes:
#   diff    (--diff REF) — run the same file with the interpreter and with REF
#                        (CRuby), compare stdout line-by-line.  Each `p` line is
#                        one assertion.  *Crash recovery*: if the interpreter
#                        SEGVs partway through a file, the offending assertion is
#                        located (by how many output lines were emitted), blanked
#                        out, and the file is re-run so the REMAINING assertions
#                        still execute.  Reported as per-assertion PASS/FAIL/CRASH
#                        instead of losing the whole file.
#   trailer (--runner R) — load R before the file (mspec shim); file PASSes iff
#                        its `pass=N fail=N err=N` trailer has fail==0 && err==0.
#
# Usage:
#   ruby run_specs.rb --interp ./koruby --diff ruby --dir t --pattern '*.rb'
#   ruby run_specs.rb --interp ./koruby --runner run_one.rb --dir spec/ruby/core
#
require 'open3'
require 'etc'

MAX_RECOVER = 30  # give up per-line recovery past this many crashing lines/file

opts = { timeout: 15, jobs: Etc.nprocessors, runner: nil, diff: nil, pattern: nil }
args = ARGV.dup
until args.empty?
  case (a = args.shift)
  when '--interp'  then opts[:interp]  = args.shift
  when '--runner'  then opts[:runner]  = args.shift
  when '--dir'     then opts[:dir]     = args.shift
  when '--diff'    then opts[:diff]    = args.shift
  when '--timeout' then opts[:timeout] = Integer(args.shift)
  when '--jobs'    then opts[:jobs]    = Integer(args.shift)
  when '--pattern' then opts[:pattern] = args.shift
  else abort "unknown option: #{a}"
  end
end
abort 'need --interp CMD' unless opts[:interp]
abort 'need --dir DIR'    unless opts[:dir]

interp  = opts[:interp].split            # may be multi-word ("env V=1 ./koruby")
ref     = opts[:diff]&.split
pattern = opts[:pattern] || (opts[:diff] ? '*.rb' : '*_spec.rb')
files   = Dir.glob(File.join(opts[:dir], '**', pattern)).sort
abort "no files matching #{pattern} under #{opts[:dir]}" if files.empty?

# Run one command under a hard wall-clock limit, isolated.  `timeout` returns
# 124 on expiry and 128+signal when the child dies by a signal (139 = SEGV).
def run(cmd, secs)
  out, _err, st = Open3.capture3('timeout', '-k', '2', secs.to_s, *cmd)
  # `timeout` propagates a signal-killed child by killing itself with the same
  # signal, so st.exitstatus is nil for a SEGV — fold it back to 128+signo
  # (the shell convention) so callers can detect crashes uniformly.
  code = st.exited? ? st.exitstatus : (st.signaled? ? 128 + st.termsig : 1)
  [out, code]
end

# Diff mode with crash recovery → { pass:, fail:, crash:, status:, crash_lines: }
#   status :ok          — completed (pass/fail/crash assertion counts valid)
#          :whole_crash — SEGV that couldn't be localized (not all `p` lines, or
#                         exceeded MAX_RECOVER); the file's tail is lost
#          :timeout
def classify_diff(file, interp, ref, secs, tmp)
  src        = File.readlines(file, chomp: true)
  exec_lines = src.each_index.select { |i| s = src[i].strip; !s.empty? && !s.start_with?('#') }
  # Recovery needs a 1:1 line↔assertion mapping; the generated corpus is all `p …`.
  recoverable = !exec_lines.empty? && exec_lines.all? { |i| s = src[i].lstrip; s.start_with?('p ', 'p(') }
  skip = []
  crash_lines = []
  loop do
    content = src.dup
    skip.each { |i| content[i] = '' } # blank a crashing line (no output, no crash)
    File.write(tmp, content.join("\n") + "\n")
    out, code = run(interp + [tmp], secs)
    return { status: :timeout } if code == 124
    if code && code >= 128 # SEGV / signal
      if recoverable && skip.size < MAX_RECOVER
        live    = exec_lines - skip
        emitted = out.lines.size
        if emitted < live.size       # the (emitted+1)-th live assertion crashed
          skip << live[emitted]
          crash_lines << live[emitted] + 1 # 1-based source line
          next
        end
      end
      return { status: :whole_crash, crash_lines: crash_lines }
    end
    # completed: diff vs CRuby on the SAME (skipped) content so lines align.
    ref_out, = run(ref + [tmp], secs)
    a = out.lines
    b = ref_out.lines
    pass = fail = 0
    [a.size, b.size].max.times { |i| a[i] == b[i] ? pass += 1 : fail += 1 }
    return { status: :ok, pass: pass, fail: fail, crash: crash_lines.size, crash_lines: crash_lines }
  end
end

# Trailer (runner) mode → { status: :pass/:fail/:err/:crash/:timeout }
def classify_trailer(file, interp, runner, secs)
  out, code = run(interp + [runner, file], secs)
  return { status: :timeout } if code == 124
  return { status: :crash }   if code && code >= 128
  if (m = out.match(/pass=(\d+)\s+fail=(\d+)\s+err=(\d+)/))
    { status: (m[2].to_i.zero? && m[3].to_i.zero?) ? :pass : :fail }
  else
    { status: :err }
  end
end

# Parallel worker pool (subprocess waits release the GVL).
queue   = Queue.new.tap { |q| files.each { |f| q << f } }
results = {}
mutex   = Mutex.new
tick    = ->(c) { mutex.synchronize { $stderr.print(c) } }
workers = Array.new([opts[:jobs], files.size].min) do |w|
  Thread.new do
    tmp = "#{ENV['TMPDIR'] || '/tmp'}/rs_#{Process.pid}_#{w}.rb"
    begin
      loop do
        file = (queue.pop(true) rescue break)
        r = if opts[:diff]
              classify_diff(file, interp, ref, opts[:timeout], tmp)
            else
              classify_trailer(file, interp, opts[:runner], opts[:timeout])
            end
        mutex.synchronize { results[file] = r }
        tick.call({ ok: (r[:fail].to_i + r[:crash].to_i).zero? ? '.' : (r[:crash].to_i > 0 ? 'x' : 'F'),
                    whole_crash: 'X', timeout: 'T', pass: '.', fail: 'F', err: 'E', crash: 'X' }[r[:status]] || '?')
      end
    ensure
      File.delete(tmp) if File.exist?(tmp)
    end
  end
end
workers.each(&:join)
$stderr.puts

# Tally.  Diff mode counts ASSERTIONS (pass/fail/crash) + files for whole-crash
# / timeout; trailer mode counts FILES per verdict.
base  = opts[:dir].chomp('/')
tally = Hash.new { |h, k| h[k] = Hash.new(0) }
cat_of = lambda do |file|
  rel = file.delete_prefix(base + '/')
  rel.include?('/') ? rel[%r{\A[^/]+}] : '(root)'
end

if opts[:diff]
  results.each do |file, r|
    c = cat_of.call(file)
    case r[:status]
    when :ok
      tally[c][:pass] += r[:pass]; tally[c][:fail] += r[:fail]; tally[c][:crash] += r[:crash]
      tally[:TOTAL][:pass] += r[:pass]; tally[:TOTAL][:fail] += r[:fail]; tally[:TOTAL][:crash] += r[:crash]
    when :whole_crash
      tally[c][:wcrash] += 1; tally[:TOTAL][:wcrash] += 1
    when :timeout
      tally[c][:timeout] += 1; tally[:TOTAL][:timeout] += 1
    end
  end
  cols = %i[pass fail crash wcrash timeout]
  puts format('%-18s %8s %6s %6s %7s %6s', 'category', 'PASS', 'FAIL', 'CRASH', 'WCRASH', 'TIMEO')
  tally.sort_by { |k, _| k == :TOTAL ? 'zzz' : k.to_s }.each do |c, h|
    puts format('%-18s %8d %6d %6d %7d %6d', c, *cols.map { |k| h[k] })
  end
  # TODO lists
  recov = results.select { |_, r| r[:status] == :ok && r[:crash].to_i > 0 }
  unless recov.empty?
    puts "\nrecovered CRASH assertions (file: lines) — #{recov.values.sum { |r| r[:crash] }} total:"
    recov.sort.first(40).each { |f, r| puts "  #{f}: #{r[:crash_lines].join(', ')}" }
  end
  %i[whole_crash timeout].each do |st|
    bad = results.select { |_, r| r[:status] == st }.keys.sort
    next if bad.empty?
    puts "\n#{st.to_s.upcase} (#{bad.size}):"
    bad.first(40).each { |f| puts "  #{f}" }
  end
  t = tally[:TOTAL]
  exit(t[:fail] + t[:crash] + t[:wcrash] + t[:timeout] == 0 ? 0 : 1)
else
  results.each { |file, r| c = cat_of.call(file); tally[c][r[:status]] += 1; tally[:TOTAL][r[:status]] += 1 }
  cols = %i[pass fail err crash timeout]
  puts format('%-18s %6s %6s %6s %6s %6s', 'category', 'PASS', 'FAIL', 'ERR', 'CRASH', 'TIMEO')
  tally.sort_by { |k, _| k == :TOTAL ? 'zzz' : k.to_s }.each do |c, h|
    puts format('%-18s %6d %6d %6d %6d %6d', c, *cols.map { |k| h[k] })
  end
  %i[crash timeout err fail].each do |st|
    bad = results.select { |_, r| r[:status] == st }.keys.sort
    next if bad.empty?
    puts "\n#{st.to_s.upcase} (#{bad.size}):"
    bad.first(40).each { |f| puts "  #{f}" }
  end
  t = tally[:TOTAL]
  exit(t[:fail] + t[:err] + t[:crash] + t[:timeout] == 0 ? 0 : 1)
end
