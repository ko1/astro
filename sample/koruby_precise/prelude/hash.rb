# Hash#to_proc — a proc that looks up its argument as a key.
class Hash
  def to_proc; h = self; ->(k) { h[k] }; end
end
