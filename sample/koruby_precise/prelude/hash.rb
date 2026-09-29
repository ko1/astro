# Hash#to_proc — a proc that looks up its argument as a key.
class Hash
  # the blockless form of the C #map / #flat_map / #reverse_each / #cycle
  private def __enum_nb(meth, *args)
    if meth == :cycle
      enum_for(:cycle, *args) do
        n = args[0]
        empty? ? 0 : n.nil? ? Float::INFINITY : [n.to_int, 0].max * size
      end
    else
      enum_for(meth) { size }
    end
  end
  # The persistent ruby2_keywords mark survives binding to a named parameter (only
  # a splat call site turns it back into keywords), so plain methods will do; being
  # real methods, respond_to? / method see them (grape asks before calling).
  class << Hash
    def ruby2_keywords_hash?(hash)
      raise TypeError, "wrong argument type #{hash.class} (expected Hash)" unless hash.is_a?(Hash)
      hash.__send__(:__kwargs_marked?)
    end

    def ruby2_keywords_hash(hash)
      raise TypeError, "wrong argument type #{hash.class} (expected Hash)" unless hash.is_a?(Hash)
      hash.dup.__send__(:__kwargs_mark!)
    end
  end

  def to_proc; h = self; ->(k) { h[k] }; end
end
