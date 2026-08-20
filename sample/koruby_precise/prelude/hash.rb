# Hash#to_proc — a proc that looks up its argument as a key.
class Hash
  # The keyword-argument mark (KORB_FL_KWARGS): a Hash written as keywords at a
  # call site keeps it through *args splat, which is what ruby2_keywords needs.
  def self.ruby2_keywords_hash?(h)
    raise TypeError, "wrong argument type #{h.class} (expected Hash)" unless h.is_a?(Hash)
    h.__kwargs_marked?
  end

  def self.ruby2_keywords_hash(h)
    raise TypeError, "wrong argument type #{h.class} (expected Hash)" unless h.is_a?(Hash)
    h.dup.__kwargs_mark!
  end

  def to_proc; h = self; ->(k) { h[k] }; end
end
