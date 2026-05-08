#!/usr/bin/env ruby
# Run the official JSON-Schema-Test-Suite (draft-07) against arjsv.
#
# Pass the suite path via env: SUITE_PATH=/path/to/JSON-Schema-Test-Suite
# defaults to /tmp/jsts/json-schema-org-JSON-Schema-Test-Suite-*/tests/draft7

require 'json'
$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'arjsv'

suite_path = ENV['SUITE_PATH']
if suite_path.nil? || !File.directory?(suite_path)
  cands = Dir.glob('/tmp/jsts/json-schema-org-*/tests/draft7')
  suite_path = cands.first
end
abort "no suite path; set SUITE_PATH=" unless suite_path && File.directory?(suite_path)

filter_files = ARGV.empty? ? nil : ARGV
total = pass = fail_count = err = skipped = 0
fails = []

files = Dir.glob(File.join(suite_path, '**/*.json')).sort
files.each do |path|
  name = path.sub(suite_path + '/', '')
  next if filter_files && !filter_files.any? { |f| name.include?(f) }
  begin
    cases = JSON.parse(File.read(path))
  rescue JSON::ParserError => e
    warn "skip #{name}: parse error #{e.message}"
    next
  end
  cases.each do |c|
    schema = c['schema']
    desc   = c['description']
    sch = nil
    begin
      sch = Arjsv.schema(schema)
    rescue Exception => e
      skipped += c['tests'].size
      fails << "#{name}: #{desc}: BUILD-SKIP #{e.class}: #{e.message[0..100]}"
      next
    end
    c['tests'].each do |t|
      total += 1
      begin
        actual = sch.valid?(t['data'])
      rescue Exception => e
        err += 1
        fails << "#{name}: #{desc} / #{t['description']}: ERROR #{e.class}: #{e.message}"
        next
      end
      expected = t['valid']
      if actual == expected
        pass += 1
      else
        fail_count += 1
        fails << "#{name}: #{desc} / #{t['description']}: expected #{expected} got #{actual}"
      end
    end
  end
end

puts "TOTAL: #{total}, PASS: #{pass}, FAIL: #{fail_count}, ERR: #{err}, SKIPPED: #{skipped}"
puts ""
puts "rate: #{(pass * 100.0 / total).round(2)}%" if total > 0
unless fails.empty?
  puts ""
  puts "--- failure groups (file → count) ---"
  groups = fails.group_by { |f| f.split(':').first }
  groups.sort_by { |_, fs| -fs.size }.each { |file, fs| puts "  %4d  %s" % [fs.size, file] }
  if ENV['SHOW_FAILS']
    puts ""
    puts "--- all failures ---"
    fails.each { |f| puts f }
  end
end
