require_relative "../../test_helper"

# public_send refuses private/protected methods; send/__send__ allow.

class PSHost
  def pub_method; :public_value; end
  private
  def priv_method; :private_value; end
  protected
  def prot_method; :protected_value; end
end

# ---------- send: visibility-blind ----------

def test_send_calls_public
  assert_equal :public_value, PSHost.new.send(:pub_method)
end

def test_send_calls_private
  assert_equal :private_value, PSHost.new.send(:priv_method)
end

def test_send_calls_protected
  assert_equal :protected_value, PSHost.new.send(:prot_method)
end

def test___send___equivalent_to_send
  assert_equal :private_value, PSHost.new.__send__(:priv_method)
end

# ---------- public_send: refuses non-public ----------

def test_public_send_calls_public
  assert_equal :public_value, PSHost.new.public_send(:pub_method)
end

def test_public_send_refuses_private
  raised = false
  begin
    PSHost.new.public_send(:priv_method)
  rescue NoMethodError, NameError
    raised = true
  end
  assert raised, "expected NoMethodError on public_send to private method"
end

def test_public_send_refuses_protected
  raised = false
  begin
    PSHost.new.public_send(:prot_method)
  rescue NoMethodError, NameError
    raised = true
  end
  assert raised, "expected NoMethodError on public_send to protected method"
end

TESTS = [
  :test_send_calls_public,
  :test_send_calls_private,
  :test_send_calls_protected,
  :test___send___equivalent_to_send,
  :test_public_send_calls_public,
  :test_public_send_refuses_private,
  :test_public_send_refuses_protected,
]
TESTS.each { |t| run_test(t) }
report "PublicSend"
