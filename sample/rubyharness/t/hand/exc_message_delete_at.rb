e = RuntimeError.new("hi")
p e.message
class MyErr < StandardError; def to_s; "this is from #to_s"; end; end
p MyErr.new("you won't see this").message
p MyErr.new.message
a = [1, 2, 3, 4]
class TI; def to_int; 2; end; end
p a.delete_at(TI.new)
p a
def t; yield; rescue FrozenError; "FE"; rescue TypeError; "TE"; end
p t { [1, 2].freeze.delete_at(0) }
p t { [1, 2].delete_at(Object.new) }
