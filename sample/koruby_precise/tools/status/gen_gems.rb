# Per-gem table for sample/koruby_precise/docs/status.html.
#   ruby tools/status/gen_gems.rb docs/status.html <label>=<tsv>[@<commit>@<date>] ...
# Each tsv row: gem, file, cruby_summary, korb_summary, korb_first_error, ...
# A file "matches" when koruby's summary line equals CRuby's (same counts of
# examples / failures / errors; a CRuby failure koruby reproduces counts as a match).
# The JSON array replaces the text between `/*GEMS:BEGIN*/` and `/*GEMS:END*/`.
require "json"

html, *specs = ARGV
abort "usage: gen_gems.rb <status.html> <label>=<tsv>@<commit>@<date> ..." if specs.empty?
gems = {}
specs.each do |spec|
  label, rest = spec.split("=", 2)
  path, commit, date = rest.split("@")
  File.foreach(path) do |line|
    g, f, cr, ko, err = line.chomp.split("\t")
    next unless f
    e = (gems[[label, g]] ||= { gem: g, corpus: label, commit: commit, date: date, files: 0, match: 0, bad: [] })
    e[:files] += 1
    if cr == ko
      e[:match] += 1
    else
      e[:bad] << { file: f, cruby: cr, koruby: ko, error: (err || "")[0, 120] }
    end
  end
end
rows = gems.values.sort_by { |e| [e[:match] == e[:files] ? 1 : 0, e[:gem].downcase] }
json = JSON.generate(rows)
src = File.read(html)
src.sub!(%r{/\*GEMS:BEGIN\*/.*?/\*GEMS:END\*/}m) { "/*GEMS:BEGIN*/#{json}/*GEMS:END*/" } or abort "no GEMS markers in #{html}"
File.write(html, src)
tot = rows.sum { _1[:files] }; ok = rows.sum { _1[:match] }
puts "#{rows.size} gem entries, #{ok}/#{tot} files match, #{rows.count { _1[:match] == _1[:files] }} gems fully match"
