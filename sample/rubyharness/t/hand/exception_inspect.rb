p Exception.new.inspect
p RuntimeError.new("boom").inspect
p ArgumentError.new("bad").inspect
class MyErr < StandardError; end
p MyErr.new("x").inspect
p MyErr.new.inspect
p StandardError.new("").inspect
