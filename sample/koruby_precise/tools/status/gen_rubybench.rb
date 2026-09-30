# Per-benchmark ruby-bench table for sample/koruby_precise/docs/status.html.
#   ruby tools/status/gen_rubybench.rb docs/status.html <benchmarks.list> <label>=<log-dir>@<commit>@<date> ...
# Log dirs are run_all_sp4.sh outputs (<bench>.<mode>.log with ruby-bench's
# "Average of last N, non-warmup iters: X ms", bake.<bench>.time).  Later runs win for
# a mode they measured, so list them oldest first.  A value that the harness did not
# print stays null (shown as "-"), never 0.
# The JSON replaces the text between `/*RBENCH:BEGIN*/` and `/*RBENCH:END*/`.
require "json"

html, list, *specs = ARGV
abort "usage: gen_rubybench.rb <status.html> <benchmarks.list> <label>=<dir>@<commit>@<date> ..." if specs.empty?
MODES = { "cruby" => "c", "yjit" => "y", "koruby" => "k", "koruby_aot" => "a" }
runs = specs.map do |spec|
  label, rest = spec.split("=", 2)
  dir, commit, date = rest.split("@")
  { label: label, dir: dir, commit: commit, date: date }
end
rows = File.readlines(list).map { |l| l.split("\t").first }.map do |b|
  row = { b: b }
  runs.each do |r|
    MODES.each do |mode, key|
      f = File.join(r[:dir], "#{b}.#{mode}.log")
      next unless File.exist?(f)
      ms = File.read(f, encoding: "BINARY")[/Average of last \d+, non-warmup iters: ([\d.]+)ms/, 1]
      row[key] = ms && ms.to_f.round(1)
      row["#{key}r"] = r[:label]
    end
    t = File.join(r[:dir], "bake.#{b}.time")
    next unless File.exist?(t)
    tx = File.read(t)
    row[:bake] = tx[/bake ([\d.]+) s/, 1]&.to_f&.round
    row[:bake_ok] = tx[/^exit (\d+)/, 1] == "0"
    row[:bakr] = r[:label]
  end
  row
end
out = { runs: runs.map { |r| r.slice(:label, :commit, :date) }, rows: rows }
src = File.read(html)
src.sub!(%r{/\*RBENCH:BEGIN\*/.*?/\*RBENCH:END\*/}m) { "/*RBENCH:BEGIN*/#{JSON.generate(out)}/*RBENCH:END*/" } or abort "no RBENCH markers in #{html}"
File.write(html, src)
puts "#{rows.size} benchmarks from #{runs.map { _1[:label] }.join(', ')}"
