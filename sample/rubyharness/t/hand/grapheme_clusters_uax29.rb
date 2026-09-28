# String#grapheme_clusters follows UAX #29 like /\X/ (combining dakuten, halfwidth voiced mark)
[ "か\u3099き\u3099", "ｶﾞｷﾞ", "e\u0301x", "🇯🇵🇺🇸", "👨‍👩‍👧x", "\r\na", "a\u0308\u0301b" ].each { |s| p [s.grapheme_clusters, s.scan(/\X/) == s.grapheme_clusters] }
