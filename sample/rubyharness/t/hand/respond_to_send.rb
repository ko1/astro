# send / __send__ / public_send are special-dispatched but every object still
# responds to them (like new on a class). vs ruby.
p Object.new.respond_to?(:send)
p Object.new.respond_to?(:__send__)
p Object.new.respond_to?(:public_send)
p 5.respond_to?(:send)
p "x".respond_to?(:__send__)
p [].respond_to?(:public_send)
p :sym.respond_to?(:send)
p 3.14.respond_to?(:send)
p nil.respond_to?(:send)
p Class.new.respond_to?(:new)
p Object.new.respond_to?(:definitely_not_a_method)
