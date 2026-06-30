$log = []
class Base; def self.inherited(sub); $log << "inh #{sub}"; end; end
class D1 < Base; end
class D2 < Base; end
p $log
$plog = []
module Prep; def self.prepended(b); $plog << "prep #{b}"; end; def m; "P+" + super; end; end
class WithP; prepend Prep; def m; "base"; end; end
p $plog
p WithP.new.m
class Animal; end
class Dog < Animal; end
p Dog.superclass
p Dog.ancestors.first(2)
class Registry
  @subs = []
  class << self; attr_reader :subs; end
  def self.inherited(s); @subs << s.name; super; end
end
class A < Registry; end
class B < Registry; end
p Registry.subs
module Loud; def self.prepended(b); end; def speak; super.upcase; end; end
class Cat; prepend Loud; def speak; "meow"; end; end
p Cat.new.speak
p Cat.ancestors.first(2)
