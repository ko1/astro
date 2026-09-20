# Unicode property names CRuby knows that astrogre's generated table was
# missing.  rdoc/text.rb is /[\p{Nd}\p{Lc}\p{Pc}]|[!-~&&\W]/ — \p{Lc} (the
# LC "cased letter" general category) made the whole file a SyntaxError.
NAMES = %w[LC Lc Cased_Letter Combining_Mark Unassigned
           Other_Alphabetic Other_Lowercase Other_Uppercase Other_Math
           Other_ID_Start Other_ID_Continue Other_Grapheme_Extend
           Other_Default_Ignorable_Code_Point Prepended_Concatenation_Mark
           IDS_Binary_Operator IDS_Trinary_Operator
           Changes_When_Casefolded Changes_When_Casemapped
           Changes_When_Lowercased Changes_When_Titlecased
           Changes_When_Uppercased]

SUBJECTS = ["a", "A", "Ω", "5", "_", "-", " ", "ǅ", "́", "Ⅰ", "⿰"]

NAMES.each do |n|
  re = Regexp.new("\\p{#{n}}")
  puts "#{n}\t#{SUBJECTS.map { |s| (s =~ re) ? 1 : 0 }.join}"
rescue RegexpError => e
  puts "#{n}\tERR #{e.message}"
end

p(/[\p{Nd}\p{Lc}\p{Pc}]|[!-~&&\W]/ =~ "a")
p(/[\p{Nd}\p{Lc}\p{Pc}]|[!-~&&\W]/ =~ "9")
p(/[\p{Nd}\p{Lc}\p{Pc}]|[!-~&&\W]/ =~ "+")
p(/[\p{Nd}\p{Lc}\p{Pc}]|[!-~&&\W]/ =~ "あ")
p("ǅ".match?(/\p{Lt}/))
p("ǅ".match?(/\p{LC}/))
