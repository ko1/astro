#!/usr/bin/env ruby
# arcel vs cel-go (and arcel interp vs AOT) benchmark.
#
# Both binaries expose a `bench` subcommand:
#
#   <bin> bench -e '<expr>' [-i '<json>'] -n <iterations>
#
# It compiles once, warms briefly, then loops <iterations> times and
# prints "<iters> <elapsed_ns> <ns_per_op>" on stdout.  We invoke each
# binary that way for every (expression, mode) tuple and tabulate.
#
# Bench cases are deliberately picked to cover the dimensions where
# AOT specialization should pay off (or not) — see CASES below.
#
# Usage:
#   ruby benchmark/run.rb                 # all cases, all modes
#   ruby benchmark/run.rb --case fib      # subset by name
#   ruby benchmark/run.rb --skip celgo    # arcel-only
#   ruby benchmark/run.rb --iters 5_000_000

require 'optparse'
require 'open3'
require 'json'

HERE = File.expand_path(__dir__)
ROOT = File.expand_path('..', HERE)

opts = {
  iters: 1_000_000,
  case_filter: nil,
  modes: %i[arcel_interp arcel_aot celgo celcpp],
  arcel_bin:  File.join(ROOT, 'arcel'),
  celgo_bin:  File.join(ROOT, 'test', 'celgo_ref', 'celgo_ref'),
  celcpp_bin: File.join(ROOT, 'test', 'celcpp_ref', 'celcpp_bench'),
  skip: [],
}
OptionParser.new do |o|
  o.on('--iters N', Integer, "iterations per measurement (default #{opts[:iters]})") { |v| opts[:iters] = v }
  o.on('--case REGEX',         'only run cases whose name matches')                  { |v| opts[:case_filter] = Regexp.new(v) }
  o.on('--arcel PATH',         'path to arcel binary')                               { |v| opts[:arcel_bin] = v }
  o.on('--celgo PATH',         'path to celgo_ref binary')                           { |v| opts[:celgo_bin] = v }
  o.on('--celcpp PATH',        'path to celcpp_bench binary')                        { |v| opts[:celcpp_bin] = v }
  o.on('--skip MODES',         'comma-sep modes to skip (arcel_interp,arcel_aot,celgo,celcpp)') { |v| opts[:skip] = v.split(',').map(&:to_sym) }
end.parse!

opts[:modes] -= opts[:skip]

# ---- bench cases ---------------------------------------------------------
#
# Aim: cover the axes where AOT specialization should and shouldn't win.
#
#   - tiny scalar      → dispatch overhead is most of the cost
#   - deep nesting     → many dispatched nodes per eval
#   - predicate w/ vars→ field access dominates → AOT specializes path
#   - list iteration   → loop body called many times → big win possible
#   - string ops       → C-API call (regex/contains) dominates → small win
#   - boolean ladder   → short-circuit, branch-prediction sensitive

CASES = [
  {
    name: 'arith_const',
    expr: '1 + 2 * 3 - 4 / 2 + 5 % 3',
    input: nil,
    note: 'tiny constant arithmetic — pure dispatch overhead',
  },
  {
    name: 'bool_ladder',
    expr: 'true && false || true && true && (false || true)',
    input: nil,
    note: 'short-circuit boolean — branch-predictor friendly',
  },
  {
    name: 'field_access_shallow',
    expr: 'x.a + x.b + x.c',
    input: { 'x' => { 'a' => 1, 'b' => 2, 'c' => 3 } },
    note: 'shallow object access — hash lookup × 3',
  },
  {
    name: 'field_access_deep',
    expr: 'x.a.b.c.d + x.a.b.c.e',
    input: { 'x' => { 'a' => { 'b' => { 'c' => { 'd' => 10, 'e' => 32 } } } } },
    note: 'deep nested field access — chained lookups',
  },
  {
    name: 'predicate_user',
    expr: 'u.age >= 18 && u.country == "JP" && u.role in ["admin", "user"]',
    input: { 'u' => { 'age' => 25, 'country' => 'JP', 'role' => 'admin' } },
    note: 'realistic policy predicate: age + enum + set membership',
  },
  {
    name: 'list_all_small',
    expr: 'xs.all(x, x > 0)',
    input: { 'xs' => [1, 2, 3, 4, 5] },
    note: 'list quantifier × 5 — loop body specialization',
  },
  {
    name: 'list_all_med',
    expr: 'xs.all(x, x > 0)',
    input: { 'xs' => (1..100).to_a },
    note: 'list quantifier × 100 — same body, more iters',
  },
  {
    name: 'list_exists',
    expr: 'xs.exists(x, x > 50)',
    input: { 'xs' => (1..100).to_a },
    note: 'exists — short-circuit on first true',
  },
  {
    name: 'string_starts',
    expr: 'email.endsWith("@example.com")',
    input: { 'email' => 'alice@example.com' },
    note: 'string method — single C call dominates',
  },
  {
    name: 'string_contains_ladder',
    expr: 's.contains("foo") || s.contains("bar") || s.contains("baz")',
    input: { 's' => 'hello world bar baz' },
    note: '3 string ops — interpreter dispatch counts',
  },
  {
    name: 'k8s_admission_ish',
    expr: 'object.spec.replicas <= maxReplicas && ' \
          'object.metadata.labels["team"] in allowedTeams && ' \
          'object.metadata.namespace.startsWith(envPrefix)',
    input: {
      'object'       => {
        'spec'     => { 'replicas' => 3 },
        'metadata' => { 'labels' => { 'team' => 'platform' }, 'namespace' => 'prod-payments' },
      },
      'maxReplicas'  => 5,
      'allowedTeams' => %w[platform infra security],
      'envPrefix'    => 'prod-',
    },
    note: 'realistic K8s ValidatingAdmissionPolicy expression',
  },
]

# ---- runner --------------------------------------------------------------

def run_bench(bin, args, expr, input, iters)
  # arcel parses global flags (e.g. --no-compile) BEFORE the subcommand,
  # so put `args` ahead of 'bench'.  celgo_ref ignores these flags.
  # celcpp_bench has no `bench` subcommand — it always benches.
  if bin =~ /celcpp_bench$/
    cmd = [bin] + args + ['-e', expr, '-n', iters.to_s]
  else
    cmd = [bin] + args + ['bench', '-e', expr, '-n', iters.to_s]
  end
  cmd.push('-i', JSON.generate(input)) if input
  out, err, status = Open3.capture3(*cmd)
  unless status.success?
    return { error: err.strip.empty? ? out.strip : err.strip }
  end
  # expected: "iters elapsed_ns ns_per_op\n"
  fields = out.split.last(3)
  return { error: "bad bench output: #{out.inspect}" } unless fields.size == 3
  { iters: fields[0].to_i, ns: fields[1].to_i, ns_per_op: fields[2].to_f }
end

MODE_DESC = {
  arcel_interp: { label: 'arcel-i', cmd: ->(b) { [b[:arcel_bin], ['--no-compile']] } },
  arcel_aot:    { label: 'arcel-A', cmd: ->(b) { [b[:arcel_bin], []] } },
  celgo:        { label: 'celgo',   cmd: ->(b) { [b[:celgo_bin], []] } },
  celcpp:       { label: 'celcpp',  cmd: ->(b) { [b[:celcpp_bin], []] } },
}

cases_to_run = CASES.reject { |c| opts[:case_filter] && c[:name] !~ opts[:case_filter] }

unavailable = opts[:modes].reject do |m|
  bin, _ = MODE_DESC[m][:cmd].call(opts)
  File.executable?(bin)
end
unavailable.each do |m|
  warn "skipping mode #{m}: binary not executable (#{MODE_DESC[m][:cmd].call(opts).first})"
end
opts[:modes] -= unavailable
abort 'no available modes' if opts[:modes].empty?

# ---- header --------------------------------------------------------------

puts "iters/measurement: #{opts[:iters]}"
puts "modes:             #{opts[:modes].map { |m| MODE_DESC[m][:label] }.join(', ')}"
puts ""

col_w = opts[:modes].map { |_| 14 }
header = format("%-30s  %s",
  'case', opts[:modes].each_with_index.map { |m, i| MODE_DESC[m][:label].rjust(col_w[i]) }.join('  '))
header += '   speedup' if opts[:modes].size >= 2
puts header
puts '-' * header.length

cases_to_run.each do |c|
  cells = []
  results = {}
  opts[:modes].each_with_index do |m, i|
    bin, args = MODE_DESC[m][:cmd].call(opts)
    r = run_bench(bin, args, c[:expr], c[:input], opts[:iters])
    results[m] = r
    cells << if r[:error]
               'ERR'.rjust(col_w[i])
             else
               format('%.0f ns/op', r[:ns_per_op]).rjust(col_w[i])
             end
  end

  line = format('%-30s  %s', c[:name], cells.join('  '))

  # Speedup: arcel-AOT vs celgo, or arcel-interp vs celgo, or AOT vs interp.
  if results.size >= 2
    base = results[:celgo] || results[:arcel_interp]
    fast = results[:arcel_aot] || results[:arcel_interp] || results[:celgo]
    if base && fast && !base[:error] && !fast[:error] && fast[:ns_per_op] > 0
      line += format('   %5.2fx', base[:ns_per_op] / fast[:ns_per_op])
    end
  end

  puts line
  results.each do |m, r|
    puts "    [#{MODE_DESC[m][:label]}] #{r[:error]}" if r[:error]
  end
end
