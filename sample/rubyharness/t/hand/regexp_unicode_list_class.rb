# Onigmo allows a whitespace-separated codepoint LIST in `\u{...}`, and inside a
# character class each codepoint is its own member (`[\u{61 62}]` == [ab]).
# astrogre read only one run of hex digits there and rejected the whole pattern
# with "invalid Unicode list", so rubocop's
# Style/RedundantRegexpCharacterClass spec could not even parse its fixture.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 55]]
end

t(:literal)     { /[\u{61 62}]/.source }
t(:matches_a)   { /[\u{61 62}]/.match?("a") }
t(:matches_b)   { /[\u{61 62}]/.match?("b") }
t(:not_c)       { /[\u{61 62}]/.match?("c") }
t(:three)       { ["a", "b", "c", "d"].map { |s| /[\u{61 62 63}]/.match?(s) } }
t(:multibyte)   { ["あ", "い", "う"].map { |s| /[\u{3042 3044}]/.match?(s) } }
t(:with_other)  { ["a", "b", "z"].map { |s| /[z\u{61 62}]/.match?(s) } }
t(:negated)     { ["a", "b", "c"].map { |s| /[^\u{61 62}]/.match?(s) } }
t(:ignorecase)  { ["A", "B", "C"].map { |s| /[\u{61 62}]/i.match?(s) } }
t(:single)      { /[\u{61}]/.match?("a") }
t(:range_ok)    { ["m", "Z"].map { |s| /[\u{61}-\u{7a}]/.match?(s) } }
t(:outside)     { /\u{61 62}/.match?("ab") }
t(:dynamic)     { Regexp.new("[\\u{61 62}]").match?("b") }
t(:gsub)        { "abcab".gsub(/[\u{61 62}]/, "-") }
t(:scan)        { "a1b2c".scan(/[\u{61 62 63}]/) }
t(:empty_bad)   { Regexp.new("[\\u{}]") }
# NOTE: `[\u{61}-\u{62 63}]` (a LIST at a range end) is accepted by Onigmo — the
# first codepoint closes the range and the rest become members — but astrogre's
# range-end reader has nowhere to put the extras, so koruby still rejects it.
# Left out rather than asserted: it rejected this before the fix too.
