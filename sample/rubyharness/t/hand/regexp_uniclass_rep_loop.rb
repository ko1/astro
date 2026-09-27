# `[…]*` / `+?` / `{m,n}` over a class with non-ASCII members runs as a loop,
# not a C frame per character: rexml's VALID_XML_CHARS on megabytes of text
# no longer overflows, and greedy / lazy / bounded forms backtrack as before.
valid = /^[\t\n\r -\u{D7FF}\u{E000}-\u{FFFD}\u{10000}-\u{10FFFF}]*$/
p ("aaaa " * 1_000_000).match?(valid), ("aaaa " * 1000 + "\u0001").match?(valid)
p "あいうえお"[/[あ-う]*/], "あいうえお"[/[あ-う]+?/], "あいうえお"[/[あ-お]{2,3}/], "あいうえお"[/[あ-お]{2,3}?/]
p "xあいあいy"[/[あい]*い/], "xあいあいy"[/[あい]+?い/], "xあいあいy" =~ /[あい]{5}/, "aéaéb"[/[aé]*é/]
p "ｘ１２３ｙ".scan(/[０-９]+/), "日本語text".sub(/\A[一-龯]*/, "<\\0>"), "ααβ".gsub(/[α]*/, "-")
p "abc" =~ /[aé]*c/, "é" * 3 =~ /\A[é]{4}/, ("あ" * 200_000)[/[あい]+\z/].size, ("あ" * 200_000 + "x").match?(/\A[あい]*?x\z/)
