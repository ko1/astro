# 正規表現の \u{...} は「空白区切りのコードポイント列」を取り、その連結にマッチする。
# astrogre は 1 個しか読まず SyntaxError にしていた (strscan の test が丸ごと落ちた)。
p(/\u{30c6 30b9 30c8}/ =~ "これはテストです")
p(/\u{41}/ =~ "A")
p(/\u{41 42}/ =~ "xAB")
p(/\u{41 42 43}/.source)
p("\u{41 42 43}")
p(/[\u{41}-\u{43}]/ =~ "B")
p(/\u{1F600}/ =~ "a\u{1F600}")
p("テスト".match?(/\u{30c6}\u{30b9}\u{30c8}/))
p(/\u{30c6  30b9}/ =~ "テス")
begin; Regexp.new("\\u{}"); rescue => e; p e.class.to_s; end
begin; Regexp.new("\\u{zz}"); rescue => e; p e.class.to_s; end
