# @@x read through node_cvar_get's cache: repeated reads in one method (instance and class methods, a module's),
# subclasses and including classes, then every way the answer changes — reassignment, class_variable_set,
# remove_class_variable, a later definition in an ancestor (overtaken), include of a module defining it, GC
module Conf
  @@level = :mod
  def level = @@level
  def self.level = @@level
end
class Base
  @@count = 0
  @@name = "base"
  def bump = (@@count += 1)
  def count = @@count
  def name = @@name
  def self.count = @@count
  def self.soft = (@@soft ||= :init)
  class << self
    def sname = @@name
  end
end
class Kid < Base; def kid_count = @@count; end
class UsesConf; include Conf; end
k = Kid.new; b = Base.new
4.times do |i|
  GC.start if i == 2
  k.bump
  p b.count, k.count, k.kid_count, Base.count, Kid.count, b.name, Base.sname, Kid.sname, UsesConf.new.level, Conf.level, Base.soft
end
Base.class_variable_set(:@@name, "renamed")
p b.name, Base.sname, Kid.new.name
Base.class_variable_set(:@@fresh, 1)
class Base; def fresh = @@fresh; end
p b.fresh
Base.remove_class_variable(:@@fresh)   # reading it again here: CRuby master (910741083c) segfaults (stale cvar cache)
p Base.class_variable_defined?(:@@fresh)
class Kid; @@own = :kid; def own = @@own; end
p Kid.new.own
class Base; @@own = :base_later; end
begin
  Kid.new.own
rescue RuntimeError => e
  p e.message
end
module Extra; @@depth = :extra; end
class Plain; def depth = @@depth; end
begin
  Plain.new.depth
rescue NameError => e
  p e.class
end
class Plain; include Extra; end
p Plain.new.depth
Base.class_variable_set(:@@count, 100)
p k.count, Base.count, 3.times.map { k.bump }
