require_relative "../../test_helper"

def test_basic_rescue
  result = begin
    raise "boom"
  rescue => e
    e.message rescue e.to_s
  end
  assert_equal "boom", result
end

def test_rescue_no_var
  result = begin
    raise "x"
    "unreached"
  rescue
    "caught"
  end
  assert_equal "caught", result
end

def test_rescue_with_class
  result = begin
    raise RuntimeError, "test"
  rescue RuntimeError => e
    "rt: #{e}"
  end
  assert_equal "rt: test", result
end

def test_rescue_modifier
  x = (raise "boom") rescue "ok"
  assert_equal "ok", x
end

def test_ensure_runs_on_normal
  log = []
  begin
    log << :body
  ensure
    log << :ensure
  end
  assert_equal [:body, :ensure], log
end

def test_ensure_runs_on_raise
  log = []
  begin
    begin
      log << :body
      raise "boom"
    ensure
      log << :ensure
    end
  rescue
    log << :rescued
  end
  assert_equal [:body, :ensure, :rescued], log
end

def test_ensure_in_method
  log = []
  def m(log)
    log << :start
    raise "boom"
  ensure
    log << :ensure
  end
  begin
    m(log)
  rescue
    log << :caught
  end
  assert_equal [:start, :ensure, :caught], log
end

def test_raise_without_msg
  caught = false
  begin
    raise
  rescue
    caught = true
  end
  assert_equal true, caught
end

def test_nested_rescue
  result = begin
    begin
      raise "inner"
    rescue
      raise "outer"
    end
  rescue => e
    "got: #{e}"
  end
  assert_equal "got: outer", result
end

# Custom exception class
class MyError < StandardError; end

def test_custom_exception
  result = begin
    raise MyError, "my msg"
  rescue MyError => e
    "ME: #{e}"
  end
  assert_equal "ME: my msg", result
end

TESTS = %i[
  test_basic_rescue test_rescue_no_var test_rescue_with_class
  test_rescue_modifier test_ensure_runs_on_normal test_ensure_runs_on_raise
  test_ensure_in_method test_raise_without_msg test_nested_rescue
  test_custom_exception
]
TESTS.each {|t| run_test(t) }
report("Exception")
