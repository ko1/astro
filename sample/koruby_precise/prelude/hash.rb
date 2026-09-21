# Hash#to_proc — a proc that looks up its argument as a key.
class Hash
  # Hash.ruby2_keywords_hash / ?  are C (korb_send_impl): a Ruby definition would
  # bind the argument to a named parameter, which is where the keyword mark is
  # dropped, so the predicate could never see it.

  def to_proc; h = self; ->(k) { h[k] }; end
end
