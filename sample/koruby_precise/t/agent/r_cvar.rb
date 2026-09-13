class Host
  @@secret = "password"
  def get_binding
    a = true
    binding
  end
end
b = Host.new.get_binding
p(eval("@@secret", b))
