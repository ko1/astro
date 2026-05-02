require_relative "../../test_helper"

class Plain; end

def test_singleton_via_class_lshift
  obj = Plain.new
  class << obj
    def special; "special"; end
  end
  assert_equal "special", obj.special
end

def test_singleton_doesnt_leak_to_other_instances
  obj1 = Plain.new
  obj2 = Plain.new
  class << obj1
    def only_me; "yes"; end
  end
  assert_equal "yes", obj1.only_me
  raised = false
  begin
    obj2.only_me
  rescue
    raised = true
  end
  assert_equal true, raised
end

# def obj.method
def test_def_obj_method
  obj = Plain.new
  def obj.greet; "hi"; end
  assert_equal "hi", obj.greet
end

TESTS = [:test_singleton_via_class_lshift, :test_singleton_doesnt_leak_to_other_instances, :test_def_obj_method]
TESTS.each { |t| run_test(t) }
report "Singleton"
