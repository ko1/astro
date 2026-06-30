$log = []
class Hooked
  def self.method_added(name); $log << name; end
  def foo; end
  def bar; end
end
p $log.sort
class Tracker
  @methods = []
  class << self; attr_reader :methods; def method_added(n); @methods << n; super; end; end
  def x; end
  def y; end
end
p Tracker.methods.sort
class NoHook; def m1; end; def m2; end; end
p NoHook.instance_methods(false).sort
class Base2; def self.inherited(s); super; end; end
class Sub2 < Base2; end
p Sub2.superclass
module Validatable
  def self.included(base); base.extend(ClassMethods); end
  module ClassMethods
    def validations; @validations ||= []; end
    def validate(name); validations << name; end
  end
end
class Form
  include Validatable
  validate :presence
  validate :length
end
p Form.validations
