#!/usr/bin/env ruby
# test/run.rb — baruby_precise の test runner.
#
# 使い方:
#   ruby test/run.rb                          # default backend (= make GC=<default>) で全 test
#   ruby test/run.rb --backend copy           # 指定 backend で全 test
#   ruby test/run.rb --all                    # 全 16 backend で全 test (rebuild 含む)
#   ruby test/run.rb --stress                 # BARUBY_GC_STRESS=1 で全 test
#   ruby test/run.rb --bin ./baruby_precise   # 指定 binary
#
# 各 T_*.ba.rb を実行、 結果を test/expected/T_*.expected と比較。
# expected が無ければ none backend で生成して保存 (= oracle 生成 mode)。

require 'open3'
require 'optparse'
require 'fileutils'

ROOT = File.expand_path('..', __dir__)
TEST_DIR = File.expand_path(__dir__)
EXP_DIR = File.join(TEST_DIR, 'expected')
FileUtils.mkdir_p(EXP_DIR)

opts = {
  backend: nil,         # nil = use whatever binary is built
  all_backends: false,
  stress: false,
  bin: File.join(ROOT, 'baruby_precise'),
  regenerate_expected: false,
}
OptionParser.new do |o|
  o.on('--backend GC', 'rebuild + test single backend') { opts[:backend] = _1 }
  o.on('--all',    'rebuild + test all 16 backends')    { opts[:all_backends] = true }
  o.on('--stress', 'BARUBY_GC_STRESS=1')                { opts[:stress] = true }
  o.on('--bin PATH', 'use specific binary')             { opts[:bin] = _1 }
  o.on('--regenerate', 'regenerate test/expected/*.expected from current binary') {
    opts[:regenerate_expected] = true
  }
end.parse!

ALL_BACKENDS = %w[
  none mark mark_gen mark_gen_inc copy copy_gen copy_gen_inc
  mark_compact mark_compact_gen bump mark_bump_gen
  immix immix_gen mark_bitmap_gen mark_card_gen mark_freelist
]

TESTS = Dir[File.join(TEST_DIR, 'T_*.ba.rb')].sort

def build(backend)
  system("cd #{ROOT} && make GC=#{backend} ASTRO_DEBUG=0",
         out: '/tmp/baruby_test_build.log', err: %i[child out])
end

TEST_TIMEOUT = 60   # seconds per test — guards hangs (e.g. stress mode + unfixed backend)

def run_test(bin, test_file, stress:)
  env = stress ? { 'BARUBY_GC_STRESS' => '1' } : {}
  # `timeout --kill-after=5` SIGTERMs first, SIGKILLs 5s later if it ignores TERM.
  out, _err, status = Open3.capture3(env,
    'timeout', '--kill-after=5', TEST_TIMEOUT.to_s, bin, '--plain', test_file)
  body = out.lines.reject { |l|
    l.start_with?('Result:') || l.start_with?('__ELAPSED__') || l.start_with?('__GC_STATS__')
  }.join
  # exitstatus は signaled (= SIGSEGV 等) の場合 nil。 signo を取り出す。
  # `timeout` exits 124 on timeout, 137 on SIGKILL.
  exit_code = status.exitstatus || (128 + (status.termsig || 0))
  [body, exit_code]
end

def expected_path(test_file)
  base = File.basename(test_file, '.ba.rb')
  File.join(EXP_DIR, "#{base}.expected")
end

def test_one(bin, test_file, stress:)
  body, exit_status = run_test(bin, test_file, stress: stress)
  exp_path = expected_path(test_file)
  expected = File.exist?(exp_path) ? File.read(exp_path) : nil

  if exit_status != 0
    return [:crash, exit_status, body]
  end
  if expected.nil?
    File.write(exp_path, body)
    return [:generated, 0, body]
  end
  if body == expected
    [:pass, 0, body]
  else
    [:fail, 0, body, expected]
  end
end

def run_backend(bin, backend, stress:)
  pass = 0
  fail = 0
  crash = 0
  TESTS.each do |t|
    name = File.basename(t, '.ba.rb')
    res, exit_status, body, expected = test_one(bin, t, stress: stress)
    case res
    when :pass
      printf "  %-25s %-15s OK\n", name, backend || 'current'
      pass += 1
    when :generated
      printf "  %-25s %-15s GEN (saved expected/)\n", name, backend || 'current'
      pass += 1
    when :fail
      printf "  %-25s %-15s FAIL\n", name, backend || 'current'
      puts "    --- expected ---"
      expected.each_line { |l| puts "    #{l}" }
      puts "    --- got ---"
      body.each_line { |l| puts "    #{l}" }
      fail += 1
    when :crash
      printf "  %-25s %-15s CRASH (exit %d)\n", name, backend || 'current', exit_status
      crash += 1
    end
  end
  [pass, fail, crash]
end

total_pass = 0
total_fail = 0
total_crash = 0

backends = opts[:all_backends] ? ALL_BACKENDS : (opts[:backend] ? [opts[:backend]] : [nil])

backends.each do |bk|
  if bk
    print "=== Build GC=#{bk} "
    if !build(bk)
      puts 'FAIL'
      next
    end
    puts 'OK'
  end
  p, f, c = run_backend(opts[:bin], bk, stress: opts[:stress])
  total_pass += p
  total_fail += f
  total_crash += c
end

if opts[:stress]
  puts "  (stress mode: BARUBY_GC_STRESS=1 — collect on every alloc)"
end
puts ""
puts "summary: pass=#{total_pass} fail=#{total_fail} crash=#{total_crash}"
exit(total_fail + total_crash > 0 ? 1 : 0)
