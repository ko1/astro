# A repetition deep enough to trip the C-stack guard is redone on a large dedicated
# stack instead of raising RegexpError or returning a lower-priority match (rexml's
# ENTITYVALUE on 200k characters); also inside a Thread and a Fiber.
def show(re, s)
  m = re.match(s)
  m ? [m.begin(0), m.end(0), (1...m.size).map { |i| [m.begin(i), m.end(i)] }] : nil
rescue => e
  e.class.to_s
end
N = 40_000
cases = [
  [/"(?:[^%&"]|%[a-z]+;|&[a-z]+;)*"/, '"' + ">" * N + '"'],
  [/"(?:[^%&"]|%[a-z]+;|&[a-z]+;)*"/, '"' + ">&ab;%c;" * (N/8) + '"'],
  [/"((?:[^<&"]|&[a-z]+;)*)"/, 'x"' + "a&b;" * (N/4) + '"y'],
  [/'([^%&']|%[a-z]+;|&[a-z]+;)*'/, "'" + "q" * N + "'"],
  [/(?:ab|b)*c|ab/, "ab" * (N/2) + "c"],
  [/(?:ab|a)*b/, "ab" * (N/2)],          # backtrack into body
  [/(?:ab|a)*?c/, "ab" * (N/2) + "c"],   # lazy
  [/(?:a|ab)*c/, "ab" * (N/2) + "c"],
  [/((?:x|yz)+)(y)/, "x" * N + "yzy"],
  [/(?:(a)|(b))*$/, "ab" * (N/2)],
  [/(?:a|bc){3,}d/, "a" * N + "d"],
  [/(?:a|bc){3,5}/, "a" * N],
  [/\A(?:0|あ)*\z/, "0" * N],
  [/\A(?:0|あ)*\z/, "0あ" * (N/2)],
  [/(?:\s|<!--.*?-->)*x/m, " <!-- c -->" * (N/11) + "x"],
  [/(?:(?:a|b)(?:c|d))*e/, "ac" * (N/2) + "e"],
  [/(?:a|b)*+a/, "ab" * 100],
  [/(\w|-)*=/, "a-" * (N/2) + "="],
  [/(?:a|(?=b)b)*$/, "ab" * (N/2)],
]
cases.each_with_index { |(re, s), i| puts "#{i}: #{show(re, s).inspect[0, 200]}" }
re, s = cases[0]
p Thread.new { show(re, s) }.value, Fiber.new { show(re, s) }.resume
p ('"' + ">" * 200_000 + '"').match?(/\A"(?:[^%&"]|%[a-z]+;|&[a-z]+;)*"\z/)
