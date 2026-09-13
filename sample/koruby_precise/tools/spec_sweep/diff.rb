# Diff two sweep TSVs by per-file pass count.  Usage: ruby diff.rb a.tsv b.tsv
def load(p)
  h = {}
  File.readlines(p).each do |l|
    c = l.chomp.split("\t")
    next if c.size < 2
    h[c[0]] = (c[1] == "WFAIL") ? :wfail : [c[1].to_i, c[2].to_i, c[3].to_i, c[4].to_i]
  end
  h
end
a = load(ARGV[0]); b = load(ARGV[1])
da = db = 0
(a.keys | b.keys).sort.each do |k|
  x = a[k] || :absent; y = b[k] || :absent
  next if x == y
  fx = x.is_a?(Array) ? "ex=#{x[0]} F=#{x[1]} E=#{x[2]} pass=#{x[3]}" : x.to_s
  fy = y.is_a?(Array) ? "ex=#{y[0]} F=#{y[1]} E=#{y[2]} pass=#{y[3]}" : y.to_s
  da += x[3] if x.is_a?(Array); db += y[3] if y.is_a?(Array)
  puts "%-60s %s  ->  %s" % [k, fx, fy]
end
puts "-- changed-file pass: #{da} -> #{db} (#{db - da >= 0 ? '+' : ''}#{db - da})"
