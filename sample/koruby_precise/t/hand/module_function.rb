module Driver
  module_function
  def load(x)
    "loaded:#{x}:#{helper(x)}"
  end
  def helper(x)
    x * 2
  end
end
p Driver.load(5)

module M2
  def self.explicit; "exp"; end
  module_function
  def a; "a"; end
  def b; a + "b"; end
end
p M2.explicit
p M2.a
p M2.b
