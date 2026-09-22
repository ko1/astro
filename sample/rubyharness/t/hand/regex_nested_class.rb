# A nested `[...]` inside a character class, including the Onigmo quirk where a
# `-` before one makes the nested class REPLACE the pending low operand.
CASES = {
  "[a[bc]]"              => %w[a b c z],
  "[a-c[e-g]]"           => %w[a b c d e g h],
  "[a-[bc]]"             => %w[a b c - d],
  "[x-[bc]]"             => %w[x b c -],
  "[a-[^bc]]"            => %w[a b c d -],
  "[a-w&&[^c-g]]"        => %w[a c g h w z],
  "[[:alpha:][:digit:]]" => %w[a 1 _],
  "[[[:alnum:]]-_.]"     => %w[a 1 - _ . !],
}
CASES.each do |src, subjects|
  re = Regexp.new(src)
  p [src, subjects.map { |s| !!(re =~ s) }]
end
p [/[a-[b-d]]/ =~ "c", /[a-[b-d]]/ =~ "-"]
