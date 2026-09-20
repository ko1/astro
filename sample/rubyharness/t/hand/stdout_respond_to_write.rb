# $stdout / $stderr の write 検査は #respond_to_missing? も見る (rspec の double を
# $stdout に差すテストがある)。エラーメッセージはクラス名。
class FakeIO
  def respond_to_missing?(n, priv = false) = (n == :write)
  def method_missing(n, *a) = (n == :write ? nil : super)
end
old = $stdout
$stdout = FakeIO.new
$stdout = old
p :assigned_ok
class NoWrite; end
[nil, 1, NoWrite.new].each do |v|
  begin
    $stdout = v
  rescue TypeError => e
    p e.message
  end
end
p $stdout.equal?(old)
