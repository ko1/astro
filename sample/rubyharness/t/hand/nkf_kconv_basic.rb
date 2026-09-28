require "nkf"
require "kconv"
u = "日本語テキスト"
s = NKF.nkf("-s -W", u)
e = NKF.nkf("-e", u)
p [s.encoding, s.bytesize, e.encoding, NKF.guess(s), NKF.guess(e), NKF.guess(u), NKF.guess("abc")]
p [NKF.nkf("-w", s) == u, NKF.nkf("-w -S", s) == u, NKF.nkf("--ic=Shift_JIS --oc=UTF-8", s) == u]
p [u.tosjis == s, s.toutf8 == u, Kconv.guess(e), u.kconv(Kconv::EUC, Kconv::UTF8) == e]
