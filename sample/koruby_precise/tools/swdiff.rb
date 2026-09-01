# Compare two tools/sw.rb dumps: print files whose pass count changed.
def load(p)
  h = {}
  File.readlines(p).each do |l|
    if (m = /\A(\S+)\s+ex=(\d+)\s+F=(\d+)\s+E=(\d+)\s+pass=(-?\d+)/.match(l))
      h[m[1]] = m[5].to_i
    elsif (m = /\A(\S+)\s+WFAIL/.match(l))
      h[m[1]] = :wfail
    end
  end
  h
end
a = load(ARGV[0]); b = load(ARGV[1])
(a.keys | b.keys).sort.each do |k|
  x = a[k] || :absent; y = b[k] || :absent
  next if x == y
  puts "%-58s %s -> %s" % [k, x, y]
end
