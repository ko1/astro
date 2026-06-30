module M; def self.included(base); puts "inc: #{base}"; end; end
class C; include M; end
module Concern
  def self.included(base); base.extend(ClassMethods); end
  module ClassMethods; def setting; :on; end; end
end
class Svc; include Concern; end
p Svc.setting
class Svc2; include Concern; end
p Svc2.setting
module Chain
  def self.included(base); base.include(Other); end
  module Other; def chained; :yes; end; end
end
class Multi; include Chain; end
p Multi.new.chained
$order = []
module A; def self.included(b); $order << :A; end; end
module B; def self.included(b); $order << :B; end; end
class Both; include A; include B; end
p $order
module NoHook; def m; :nh; end; end
class Plain; include NoHook; end
p Plain.new.m
module Validations
  def self.included(base); base.extend(ClassMethods); base.send(:attr_accessor, :errors); end
  module ClassMethods; def validate(*); :validated; end; end
end
class Model; include Validations; end
p Model.validate(:name)
m = Model.new; m.errors = []; p m.errors
module Trackd
  def self.included(base); base.instance_variable_set(:@flag, true); end
end
class HasFlag; include Trackd; end
p HasFlag.instance_variable_get(:@flag)
