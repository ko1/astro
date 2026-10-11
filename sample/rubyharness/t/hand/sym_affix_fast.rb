# Symbol#start_with? / end_with?: the ASCII fast path (no name String) and every case that leaves it.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message]
end
class ToS; def to_str = "fo"; end

t(:end_q)        { :valid?.end_with?("?") }
t(:end_miss)     { :valid.end_with?("?") }
t(:end_multi)    { :name=.end_with?("?", "!", "=") }
t(:end_empty)    { :abc.end_with?("") }
t(:end_long)     { :ab.end_with?("xab") }
t(:end_whole)    { :abc.end_with?("abc") }
t(:start_hit)    { :foo_bar.start_with?("foo") }
t(:start_miss)   { :foo_bar.start_with?("bar") }
t(:start_multi)  { :foo_bar.start_with?("x", "fo") }
t(:start_empty)  { :foo.start_with?("") }
t(:start_long)   { :fo.start_with?("foo") }
t(:start_re)     { [:foo_bar.start_with?(/fo+/), $~ && $~[0]] }
t(:start_re_no)  { [:foo_bar.start_with?(/bar/), $~] }
t(:start_to_str) { :foo.start_with?(ToS.new) }
t(:end_to_str)   { :xfo.end_with?(ToS.new) }
t(:end_nonstr)   { :foo.end_with?(1) }
t(:start_nonstr) { :foo.start_with?(nil) }
t(:end_mb_sym)   { :"caféé".end_with?("é") }
t(:end_mb_part)  { :"aé".end_with?("\xA9".b) }
t(:start_mb)     { :"été".start_with?("é") }
t(:end_utf16)    { :abc.end_with?("c".encode("UTF-16LE")) }
t(:start_utf16)  { :abc.start_with?("a".encode("UTF-16LE")) }
t(:end_noargs)   { :abc.end_with? }
t(:start_noargs) { :abc.start_with? }
t(:dsym)         { "dyn#{1}".to_sym.end_with?("1") }
t(:op_sym)       { :+.start_with?("+") }
t(:after_hit_bad){ :abc.end_with?("c", 1) }
