# 要素の #inspect / #to_s が上げた例外を container の #inspect が飲み込まない。
class Boom
  def inspect = self            # 非 String → CRuby は #to_s に回す
  def to_s = raise(RuntimeError, "boom")
end
class Bang
  def inspect = raise(ArgumentError, "bang")
end
class Holder
  def initialize = @x = Boom.new
end

begin; [Boom.new].inspect;   rescue => e; p [:ary,  e.class, e.message]; end
begin; ({ k: Boom.new }).inspect; rescue => e; p [:hash, e.class, e.message]; end
begin; Holder.new.inspect;   rescue => e; p [:ivar, e.class, e.message]; end
begin; p Boom.new;           rescue => e; p [:p,    e.class, e.message]; end
begin; [Bang.new].inspect;   rescue => e; p [:ary2, e.class, e.message]; end
begin; [[Bang.new]].inspect; rescue => e; p [:nest, e.class, e.message]; end

# 例外が出なければ今まで通り
class Nice
  def inspect = "NICE"
end
p [Nice.new]
p({ k: Nice.new })
p [1, "x", :y, nil, 2.5]

# 非 String の #inspect は #to_s の結果で表示される
class ViaToS
  def inspect = self
  def to_s = "VIA_TO_S"
end
p [ViaToS.new]
p ViaToS.new
