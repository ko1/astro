# MatchData#[name] / named captures: duplicate names (last participating wins), Symbol / String keys, many groups
re = /(?<y>\d{4})-(?<m>\d\d)(?:-(?<d>\d\d))?|(?<word>[a-z]+)/
m = re.match("2026-10-03")
p [m[:y], m["m"], m[:d], m[:word], m.names]
m2 = re.match("hello")
p [m2[:y], m2[:word], (m2[:nope] rescue $!.message)]
dup = /(?<x>a)|(?<x>b)/
p [dup.match("a")[:x], dup.match("b")[:x]]
long = Regexp.new((1..20).map { "(?<g#{_1}>#{_1})" }.join("-"))
lm = long.match((1..20).to_a.join("-"))
p (1..20).map { lm["g#{_1}"].to_i }.sum, lm[:g20]
p "path/to/file.rb" =~ %r{(?<dir>.+)/(?<base>[^/]+)\z} ? [$~[:dir], $~[:base]] : nil
p (m.named_captures rescue nil), m.values_at(1, 2)
