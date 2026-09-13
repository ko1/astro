# スレッド/ファイバを跨いで caller を撃ち続ける。carry が他スレッドに漏れるなら
# 鎖が壊れる (別スレッドのフレームが出る/落ちる) はず。
class K
  def +(o); c = caller(0); raise "leak: #{c.inspect}" if c.any? { |s| s.include?('other') }; 0; end
  def hash; caller(0).size; end
  def eql?(o); false; end
end
def worker(id)
  200.times do |i|
    h = {}
    h[K.new] = i
    K.new + 1
    [1, 2].each { |x| caller(0) }
    Thread.pass
  end
  id
end
ths = 4.times.map { |i| Thread.new { worker(i) } }
p ths.map(&:value)
f = Fiber.new { 100.times { |i| K.new + 1; Fiber.yield caller(0).size } }
p 50.times.map { f.resume }.uniq.size
p "ok"
