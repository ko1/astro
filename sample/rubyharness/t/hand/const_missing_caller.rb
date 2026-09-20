# const_missing / method_missing の中の caller は「参照した場所」から始まる。
# CRuby の rb_mod_const_missing は自分のフレームを畳むので、既定の NameError も
# 参照行で報告される。rspec の CallerFilter がこれに依存している。
class A
  def self.const_missing(n) = caller.first(2)
  def method_missing(n, *a) = caller.first(2)
  def respond_to_missing?(*) = true
  def normal = caller.first(2)
end
def t1 = A::Zzz
def t2 = A.new.zzz
def t3 = A.new.normal
p t1, t2, t3
module M2
  def self.const_missing(n) = caller.first(1)
end
def t4 = M2::Nope
def t5 = [1].map { M2::Nope }
p t4, t5
def t6 = Zzzz_undefined_top
begin
  t6
rescue NameError => e
  p e.backtrace.first(2)
end
