# `\cX` / `\C-X` control escapes (also as character-class range ends), and a
# bare `\p` / `\P` with no `{name}`, which CRuby warns about and then treats as
# the letter itself.
def first_byte(re)
  (0..255).find { |b| re =~ b.chr("BINARY") rescue nil }
end

%w[a z A Z ? @ [ ]].each do |ch|
  p ["\\c#{ch}", first_byte(Regexp.new("\\c#{ch}"))]
  p ["\\C-#{ch}", first_byte(Regexp.new("\\C-#{ch}"))]
end
begin
  Regexp.new("\\c\\")
rescue RegexpError => e
  p e.message
end

p Regexp.new("[\\C-A]").is_a?(Regexp)
p Regexp.new("[\\C-A-\\cz]") =~ "\x05"
p Regexp.new("[\\C-A-\\cz]") =~ "\x40"
p Regexp.new("[\\cA\\cz]") =~ "\x1a"

# CRuby warns "invalid Unicode Property" for these; koruby does not yet
# (astrogre has no warning channel), so keep stderr out of the comparison.
$VERBOSE = nil
p [Regexp.new("^\\P$") =~ "P", Regexp.new("^\\p$") =~ "p", Regexp.new("\\P") =~ "x"]
p [Regexp.new("[\\p]") =~ "p", Regexp.new("[a\\Pz]") =~ "P"]
p Regexp.new("\\p{Alpha}") =~ "a"

# an escaped multi-byte character inside a class is that character
p [Regexp.new("[\\§]") =~ "§", Regexp.new("[\\§]") =~ "x"]
p [Regexp.new("[\\–]") =~ "–", Regexp.new("[a\\–b]") =~ "b"]
p [Regexp.new("\\§") =~ "§"]
p [Regexp.new("[§]") =~ "§"]
p [Regexp.new("[\\中\\文]") =~ "文"]
p [Regexp.new("[\\u00a7]") =~ "§"]
