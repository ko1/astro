# A name may sit on SEVERAL groups — one per alternative — and `\k<name>` in a
# sub/gsub replacement has to expand to the group that actually took part.
# koruby stopped at the FIRST group with that name, so the unmatched
# alternative's (empty) capture was emitted.  rubocop's Lint/DuplicateMethods
# builds its message with exactly that pattern, so every offence came out as
# "Method `.some_method`" instead of "Method `A.some_method`".
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 50]]
end

RE = /(?:(?<name>.*)::)\#<Class:\k<name>>|\#<Class:(?<name>.*)>(?:::)?/

t(:second_alt)   { "#<Class:A>".sub(RE, '\k<name>.') }
t(:first_alt)    { "A::#<Class:A>".sub(RE, '\k<name>.') }
t(:no_match)     { "plain".sub(RE, '\k<name>.') }
t(:match_named)  { m = RE.match("#<Class:A>"); [m[:name], m[0]] }
t(:match_first)  { m = RE.match("A::#<Class:A>"); [m[:name], m[0]] }
t(:names)        { RE.names }

D = /(?<a>x)|(?<a>y)/
t(:dup_x)        { "x".sub(D, '[\k<a>]') }
t(:dup_y)        { "y".sub(D, '[\k<a>]') }
t(:dup_md)       { [D.match("x")[:a], D.match("y")[:a]] }
t(:dup_gsub)     { "xyx".gsub(D, '<\k<a>>') }

t(:plain_named)  { "xy".sub(/(?<a>x)(?<b>y)/, '\k<b>\k<a>') }
t(:numbered)     { "xy".sub(/(x)(y)/, '\2\1') }
t(:amp)          { "abc".sub(/b/, '[\&]') }
t(:plus)         { "abc".sub(/(a)(b)/, '[\+]') }
t(:backref_re)   { /(?<a>b)\k<a>/.match("bb")&.to_a }
t(:three_alts)   { r = /(?<v>1)|(?<v>2)|(?<v>3)/; ["1", "2", "3"].map { |s| s.sub(r, '(\k<v>)') } }
t(:nested_dup)   { "ab".sub(/(?<p>a)(?:(?<q>z)|(?<q>b))/, '\k<q>\k<p>') }
