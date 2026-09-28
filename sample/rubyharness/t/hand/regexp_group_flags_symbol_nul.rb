# a bare (?-x) / (?-i) inside a group ends at that group; Symbol =~ / match? / Regexp.escape see a NUL in the name
p "ab" =~ /(?:(?-x)a) b/x, "ab" =~ /(?=(?-x)a) a b/x, "ab" =~ /(?=(?-i)a)A B/ix, "Ab" =~ /(?:(?i)a)b/
re = /
  (?=(?-x)Pass a type or block to #responses\b)
  (?=.*config\.responses_without_block.*:silence_deprecation_warning\b)
     .*\#clear_responses\b
/ix
s = "w: Pass a type or block to #responses, set config.responses_without_block to :frozen_dup or :silence_deprecation_warning, or use #extract_responses or #clear_responses.\n"
p re.match?(s), s =~ re
p "a b#c" =~ /a (?-x)b#c/x, "a b" =~ /a(?-x: b)/x, "ab" =~ /a # comment
 b/x, "a#b" =~ /a(?-x)#b/x, "a b" =~ /(?x) a (?-x) b/
re = /[(){ \x00-\x1f\x7f%*"\\\]]/n
[:"with_parens()", :"with_list_wildcards*", :"with\0null", :"with\rCR", :Seen].each { |s| p [s.match?(re), s.to_s.match?(re), (s =~ re), s.match(re)&.[](0)] }
p Regexp.escape(:"a\0b.c"), :"é\0" =~ /\0/, :"日本" =~ /本/, :"a\0b".match(/b/)&.pre_match
