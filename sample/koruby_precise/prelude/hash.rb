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
  # Hash.ruby2_keywords_hash / ?  are C (korb_send_impl): a Ruby definition would
  # bind the argument to a named parameter, which is where the keyword mark is
  # dropped, so the predicate could never see it.

  def to_proc; h = self; ->(k) { h[k] }; end
end
