#!/usr/bin/env ruby
# frozen_string_literal: true
#
# gen_from_rubyspec.rb — grow the golden corpus by MINING ruby/spec.
#
# rubyspec examples look like `<expr>.should == <val>` / `expect(<expr>).to …`.
# We extract the receiver expression `<expr>`, run it standalone on CRuby, and
# keep it as a `p (<expr>)` golden line iff it executes cleanly, deterministically
# (no embedded address), and in bounded time.  Examples that need before-blocks,
# mocks, or local setup raise when run standalone and are dropped — what survives
# is the self-contained core (literal receivers), so rubyspec drives WHICH real
# Ruby behaviours we cover without us hand-writing them.  Output is grouped by
# the spec's category (the dir under core/) so `make test CAT=<area>` picks it up.
#
# Usage: ruby tools/gen_from_rubyspec.rb [SPEC_DIR] [OUT_DIR] [LINES_PER_FILE]
require 'timeout'

SPEC  = ARGV[0] || "#{ENV['HOME']}/ruby/src/master/spec/ruby/core"
OUT   = ARGV[1] || 't/spec'
CHUNK = (ARGV[2] || 80).to_i

def addr?(s) = s =~ /0x[0-9a-f]{3,}/i || s =~ /#</
def certify(expr)
  o1 = o2 = nil
  Timeout.timeout(1) { o1 = eval(expr).inspect; o2 = eval(expr).inspect } # rubocop:disable Security/Eval
  return nil if o1.nil? || o1 != o2 || o1.length > 180 || addr?(o1)
  expr
rescue Exception # NameError (setup local), SyntaxError, anything → drop
  nil
end

# Match the receiver expression of an expectation.
PATTERNS = [
  /\A\s*(.+?)\.should(?:_not)?(?:\s|\z)/,        # <expr>.should …
  /\Aexpect\((.+?)\)\.(?:to|not_to|to_not)\b/,   # expect(<expr>).to …
].freeze
# Drop expressions that depend on external setup, or are process-seeded /
# nondeterministic across runs (hash / object_id are seeded per process so they
# pass the in-process certify but differ run-to-run).
SKIP = /@|\$|\bmock\b|\bstub\b|\bScratchPad\b|\b[a-z_]\w*\s*=|->|\bself\b|\b__|
        \bhash\b|\bobject_id\b|\bequal\?|\bnil\.should\b/x

lines = Hash.new { |h, k| h[k] = [] }
seen  = {}
files = Dir.glob(File.join(SPEC, '**', '*_spec.rb'))
files.each do |path|
  cat = path[%r{/core/([^/]+)/}, 1] || path[%r{/([^/]+)/[^/]+_spec\.rb\z}, 1] || 'misc'
  File.foreach(path) do |line|
    next if line =~ /\A\s*#/
    PATTERNS.each do |re|
      m = line.match(re) or next
      expr = m[1].strip
      break if expr.empty? || expr.length > 120 || expr =~ SKIP
      key = [cat, expr]
      break if seen[key]
      seen[key] = true
      (e = certify(expr)) && (lines[cat] << "p (#{e})")
      break
    end
  end
end

require 'fileutils'
FileUtils.mkdir_p(OUT)
total = filecnt = 0
lines.sort.each do |cat, ls|
  ls.uniq!
  ls.each_slice(CHUNK).with_index do |slice, i|
    File.write(File.join(OUT, format('%s_%03d.rb', cat, i)),
               "# mined from ruby/spec: #{cat} ##{i} (oracle = CRuby)\n" + slice.join("\n") + "\n")
    filecnt += 1
    total += slice.size
  end
  warn format('  %-14s %5d', cat, ls.size) if ls.size >= 20
end
warn format('TOTAL %d mined assertions across %d files (from %d spec files)', total, filecnt, files.size)
