#!/usr/bin/env ruby
# Run Google's CEL conformance suite against an evaluator.
#
# By default the evaluator is ./arcel (the one we're building) but you
# can swap in test/celgo_ref/celgo_ref to validate that the harness
# itself is correct.  Both binaries speak the same line-protocol via
# their `repl` subcommand:
#
#   stdin :  one test per line, "expr\tjson_bindings\n"
#   stdout:  one result per line
#            • a JSON literal (true / false / null / number / string /
#              array / object) on success, OR
#            • a line beginning with "ERROR: " on parse / eval failure
#
# This way we pay process / CEL-env startup once for the whole suite
# instead of per test (cel-go startup alone is ~50 ms).
#
# Usage:
#   ruby test/run_conformance.rb                  # run ./arcel against suite
#   ruby test/run_conformance.rb --use-ref        # use cel-go reference
#   ruby test/run_conformance.rb --bin ./arcel --files basic,logic
#   ruby test/run_conformance.rb --filter 'self_eval_'
#   ruby test/run_conformance.rb -v               # show first 5 failures per file
#
# Skipped categories:
#   • Tests that depend on protobuf object literals (TestAllTypes{...})
#     are tagged :proto and skipped unless --include proto.
#   • Tests with complex container/type_env setup are flagged :env.
#   • Timestamp / duration / wrappers / any tests are :ext.
# These keep the "core CEL" run fast & focused while we build out arcel.

require 'json'
require 'open3'
require 'optparse'
require_relative 'textproto'

HERE = File.expand_path(__dir__)
DEFAULT_BIN = File.join(HERE, '..', 'arcel')
DEFAULT_REF = File.join(HERE, 'celgo_ref', 'celgo_ref')

opts = {
  bin: nil, ref: DEFAULT_REF, use_ref: false,
  files: nil, filter: nil, skip_tags: %w[proto env ext], include_tags: [],
  verbose: false, max_show: 5, list_only: false,
}
OptionParser.new do |o|
  o.banner = "Usage: ruby test/run_conformance.rb [options]"
  o.on('--bin PATH', 'evaluator binary (default ./arcel)') { |v| opts[:bin] = v }
  o.on('--ref PATH', "cel-go reference (default #{DEFAULT_REF})") { |v| opts[:ref] = v }
  o.on('--use-ref', 'evaluate via cel-go reference instead of --bin') { opts[:use_ref] = true }
  o.on('--files FILES', 'comma-separated basenames to include') { |v| opts[:files] = v.split(',') }
  o.on('--filter REGEX', 'only run tests whose full name matches') { |v| opts[:filter] = Regexp.new(v) }
  o.on('--skip TAGS', 'comma-separated tags to skip (default proto,env,ext)') { |v| opts[:skip_tags] = v.split(',') }
  o.on('--include TAGS', 'override skip for these tags') { |v| opts[:include_tags] = v.split(',') }
  o.on('-v', '--verbose', 'show failure details (up to --max-show per file)') { opts[:verbose] = true }
  o.on('--max-show N', Integer, 'failures shown per file (default 5)') { |v| opts[:max_show] = v }
  o.on('--list', 'list testable cases without running, then exit') { opts[:list_only] = true }
end.parse!

# ---- value extraction ----------------------------------------------------

# Convert a textproto Value submessage to its canonical JSON representation
# (matching what celgo_ref / arcel print on success).  Returns [json_str, kind].
# `kind` is :unsupported if we can't represent it (we'll skip rather than
# false-fail).
def value_to_json(v)
  return ['null', :null]                                     if v.has?('null_value')
  return [v.first('int64_value').to_s, :int]                 if v.has?('int64_value')
  return ["#{v.first('uint64_value')}u", :uint]              if v.has?('uint64_value')
  return [JSON.generate(v.first('bool_value')), :bool]       if v.has?('bool_value')
  if v.has?('double_value')
    d = v.first('double_value')
    return [nil, :unsupported] if d.is_a?(Float) && (d.nan? || d.infinite?)
    # textproto writes doubles inconsistently ("0" vs "0.0"); coerce
    # to Float so JSON.generate produces ".0" suffix uniformly, matching
    # arcel's print_double_json output for integer-valued doubles.
    return [JSON.generate(d.to_f), :double]
  end
  if v.has?('string_value')
    s = v.first('string_value').dup.force_encoding('UTF-8')
    s = s.b unless s.valid_encoding?    # fall back if invalid UTF-8
    return [JSON.generate(s), :string]
  end
  if v.has?('bytes_value')
    # cel-go prints bytes as a quoted Go-style escape; we'd need to
    # match exactly.  Skip until we own the comparator.
    return [nil, :unsupported]
  end
  if v.has?('list_value')
    elems = (v.first('list_value')['values'] || []).map do |e|
      js, kind = value_to_json(e)
      return [nil, :unsupported] if kind == :unsupported
      js
    end
    return ["[#{elems.join(', ')}]", :list]
  end
  if v.has?('map_value')
    entries = (v.first('map_value')['entries'] || []).map do |entry|
      ks, kk = value_to_json(entry.first('key'))
      vs, vk = value_to_json(entry.first('value'))
      return [nil, :unsupported] if kk == :unsupported || vk == :unsupported
      [ks, vs]
    end
    # CEL maps are unordered; serialize sorted by JSON key text so we
    # can do string comparison.
    entries.sort_by!(&:first)
    body = entries.map { |k, v| "#{k}: #{v}" }.join(', ')
    return ["{#{body}}", :map]
  end
  [nil, :unsupported]
end

# ---- test extraction -----------------------------------------------------

TestCase = Struct.new(:file, :section, :name, :expr, :bindings_json,
                      :expected, :expected_kind, :expect_error,
                      :tags, :raw_test, keyword_init: true)

# Heuristic tags so we can skip swathes of the suite we don't aim to support.
def file_tags(name)
  case name
  when /proto[2-9]?_?(?:ext)?\.textproto/, /enums?\.textproto/   then [:proto]
  when /timestamps?\.textproto/, /wrappers?\.textproto/          then [:ext]
  when /optionals?\.textproto/, /(network|encoders|math|string|block|bindings)_ext\.textproto/
    [:ext]
  when /dynamic\.textproto/, /type_deduction\.textproto/, /unknowns\.textproto/
    [:env]
  when /namespace\.textproto/, /fields\.textproto/, /plumbing\.textproto/
    [:env]
  else
    [:core]
  end
end

def test_tags(t, file_tags)
  tags = file_tags.dup
  expr = t.first('expr') || ''
  tags << :proto if expr =~ /\bTestAllTypes\b|\bNestedMessage\b|@type|googleapis\.com/
  tags << :proto if expr =~ /google\.protobuf\.\w+Value\b|google\.protobuf\.Value\b/
  tags << :env   if t.has?('container') || t.has?('type_env') || t.has?('disable_check')
  tags << :ext   if expr =~ /\b(timestamp|duration|optional)\b/
  tags.uniq
end

def bindings_to_json(t)
  # `bindings { key: "x" value: { value: { int64_value: 1 } } }`+
  obj = {}
  (t['bindings'] || []).each do |b|
    key = b.first('key')
    inner = b.first('value')               # ExprValue
    val = inner&.first('value')            # Value
    next unless val
    js, kind = value_to_json(val)
    return nil if kind == :unsupported
    obj[key] = JSON.parse(js)              # round-trip through Ruby for object form
  end
  obj.empty? ? '' : JSON.generate(obj)
end

cases = []
test_files = Dir[File.join(HERE, 'conformance', '*.textproto')].sort
test_files.select! { |p| opts[:files].include?(File.basename(p, '.textproto')) } if opts[:files]

test_files.each do |path|
  base = File.basename(path, '.textproto')
  ftags = file_tags(File.basename(path))
  msg = TextProto.parse_file(path)
  msg['section'].each do |sec|
    sname = sec.first('name') || ''
    sec['test'].each do |t|
      tname = t.first('name') || ''
      full = "#{base}/#{sname}/#{tname}"
      next if opts[:filter] && full !~ opts[:filter]

      ttags = test_tags(t, ftags)
      skip = (ttags & opts[:skip_tags].map(&:to_sym)) - opts[:include_tags].map(&:to_sym)
      next if skip.any?

      expr = t.first('expr')
      next unless expr
      # textproto.rb forces binary encoding internally; re-tag string
      # literals as UTF-8 (which is what CEL source actually is) so
      # JSON.generate downstream doesn't warn / refuse.
      expr = expr.dup.force_encoding('UTF-8')
      next unless expr.valid_encoding?

      bindings = bindings_to_json(t)
      next if bindings.nil?         # unsupported binding shape

      expected_str = nil
      expected_kind = nil
      expect_error = nil

      if t.has?('value')
        expected_str, expected_kind = value_to_json(t.first('value'))
        next if expected_kind == :unsupported
      elsif t.has?('eval_error')
        errs = t.first('eval_error')['errors'] || []
        expect_error = errs.first&.first('message') || ''
      else
        # No expected value & no error → typecheck-only test, skip for now
        next
      end

      cases << TestCase.new(file: base, section: sname, name: tname,
                            expr: expr, bindings_json: bindings,
                            expected: expected_str, expected_kind: expected_kind,
                            expect_error: expect_error, tags: ttags, raw_test: t)
    end
  end
end

puts "Loaded #{cases.size} runnable test cases from #{test_files.size} files"

if opts[:list_only]
  cases.each { |c| puts "#{c.file}/#{c.section}/#{c.name}" }
  exit 0
end

# ---- evaluator selection ------------------------------------------------

bin = opts[:use_ref] ? opts[:ref] : (opts[:bin] || DEFAULT_BIN)
unless File.executable?(bin)
  abort "evaluator binary not executable: #{bin}\n" \
        "  build it (make) or pass --use-ref to use the cel-go reference at #{opts[:ref]}"
end

# Both arcel and celgo_ref support `repl` mode — read one JSON envelope
# per line, write one result line.  Spawn the evaluator once and stream
# every case through it.  Use a JSON envelope (not raw TAB) so embedded
# '\n' / '\t' inside CEL string literals don't desync the protocol.
puts "Evaluator: #{bin} repl"

stdin, stdout, wait = Open3.popen2(bin, 'repl')

# Reader runs in parallel: writer can't fill OS pipe buffers and block
# while the reader is also blocked waiting for results.
results = Array.new(cases.size)
reader = Thread.new do
  cases.size.times do |i|
    line = stdout.readline
    results[i] = line.chomp
  end
end

cases.each do |c|
  envelope = { 'e' => c.expr }
  envelope['i'] = JSON.parse(c.bindings_json) unless c.bindings_json.empty?
  stdin.write(JSON.generate(envelope))
  stdin.write("\n")
end
stdin.close
reader.join
wait.value

# ---- compare -------------------------------------------------------------

passed = 0; failed = 0; errored = 0
per_file = Hash.new { |h, k| h[k] = { pass: 0, fail: 0, err: 0, fails: [] } }

cases.zip(results).each do |c, got|
  bucket = per_file[c.file]
  ok = if c.expect_error
         got.start_with?('ERROR:')
       else
         got == c.expected
       end
  if ok
    passed += 1
    bucket[:pass] += 1
  else
    if got.start_with?('ERROR:')
      errored += 1
      bucket[:err] += 1
    else
      failed += 1
      bucket[:fail] += 1
    end
    bucket[:fails] << [c, got] if bucket[:fails].size < opts[:max_show]
  end
end

puts ""
printf "%-22s  %6s %6s %6s\n", 'file', 'pass', 'fail', 'error'
puts "-" * 50
per_file.sort_by { |k, _| k }.each do |fname, b|
  printf "%-22s  %6d %6d %6d\n", fname, b[:pass], b[:fail], b[:err]
end
puts "-" * 50
printf "%-22s  %6d %6d %6d  /  %d total (%.1f%% pass)\n",
       'TOTAL', passed, failed, errored, cases.size,
       100.0 * passed / [cases.size, 1].max

if opts[:verbose]
  per_file.each do |fname, b|
    next if b[:fails].empty?
    puts "\n--- #{fname} (showing #{b[:fails].size}) ---"
    b[:fails].each do |c, got|
      puts "  #{c.section}/#{c.name}"
      puts "    expr:     #{c.expr.inspect}"
      puts "    bindings: #{c.bindings_json}" unless c.bindings_json.empty?
      puts "    expected: #{c.expect_error ? "ERROR(#{c.expect_error})" : c.expected}"
      puts "    got:      #{got}"
    end
  end
end

exit (failed + errored == 0) ? 0 : 1
