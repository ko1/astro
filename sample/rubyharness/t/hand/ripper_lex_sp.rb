# Ripper.lex reports WHITESPACE as :on_sp tokens.  rspec-support's Source (and
# rubocop/pry through it) indexes tokens by line and needs the gaps; prism's
# bundled lex_compat does not synthesise them, so koruby fills them in.
require "ripper"

CASES = {
  simple:      "2.times do\n  puts :foo\nend\n",
  cont:        "a = 1 + \\\n    2\n",
  tabs:        "def  f\t(a)\n\tp a\nend\n",
  comment:     "# c\n  x = 1  # t\n",
  crlf:        "a = 1\r\nb = 2\r\n",
  string_int:  %{s = "a\#{1 + 2}b"\n},
  empty:       "",
  only_sp:     "   \n",
  block_args:  "[1].each { |x, y| x }\n",
}
# 除外: 末尾空白の state / heredoc_end の state / `__END__` トークンの値は
# prism 側の翻訳器が本物の Ripper と元々ずれている (:on_sp とは別件)。
CASES.each { |k, v| p [k, Ripper.lex(v)] }
CASES.each { |k, v| p [k, Ripper.tokenize(v)] }

# the same source, grouped by line the way rspec-support does it
src = "2.times do\n  puts :foo\nend\n"
by_line = Ripper.lex(src).group_by { |(line, _), _, _, _| line }
p by_line.transform_values { |ts| ts.map { |_, type, _, _| type } }
