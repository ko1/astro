# multi-byte char-class members vs CRuby
p "日本語" =~ /[ぁ-ん]/
p "にほん" =~ /[ぁ-ん]/
p "カタカナ" =~ /[ァ-ヶ]/
p "x€y" =~ /[€]/
p "abcあいうdef".scan(/[あ-う]/)
p "aXbYc" =~ /[XYZあ]/
p "あ" =~ /[ぁ-ん]/
p "z" =~ /[ぁ-ん]/
p "mixあZ9".scan(/[a-zあ-ん0-9]/)
p "たちつてと".gsub(/[たと]/, "*")
p("あか" =~ /[あか]+/)
p "あかさたな"[/[あ-た]+/]
p "𠮷野家" =~ /[𠮷]/
p "café" =~ /[é]/
# ruvim の実物 regex (C1 域が multi-byte)
re = /[\u0000-\u0008\u000a-\u001f\u007f\u0080-\u009f]/
p "abc" =~ re
p "a\tb" =~ re
p "ab\u0085cd" =~ re
p "xy\u009fz".gsub(re, "?")
# range が ascii/mb 境界をまたぐ
p "ab" =~ /[~-\u0085]/
p "~" =~ /[~-\u0085]/
p "\u0085" =~ /[~-\u0085]/
p "\u0090" =~ /[~-\u0085]/
p "AあB" =~ /[\u3042]/
