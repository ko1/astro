# Single-char String/Symbol ranges iterate by codepoint ("A".."z" spans 58,
# incl. punctuation), matching CRuby. vs ruby.
p ("A".."z").to_a.size
p (:A..:z).to_a.size
p ("a".."e").to_a
p (:a..:e).to_a
p ("1".."5").to_a
p ("A"..."z").to_a.size
p ("a".."a").to_a
p (:c..:g).to_a
p ("z".."a").to_a
p ("0".."9").to_a
