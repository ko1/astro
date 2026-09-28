# hooks fired from C (inherited, singleton_method_added, const_added, ...) see the defining frame as their caller
$out = []
module H
  def self.included(b) = ($out << [:included, (caller(1, 1)[0] || 'NONE')[/:(\d+):in '(.*)'/] || 'NONE'])
  def self.extended(b) = ($out << [:extended, (caller(1, 1)[0] || 'NONE')[/:(\d+):in '(.*)'/] || 'NONE'])
  def self.prepended(b) = ($out << [:prepended, (caller(1, 1)[0] || 'NONE')[/:(\d+):in '(.*)'/] || 'NONE'])
end
class K
  def self.method_added(m) = ($out << [:method_added, (caller(1, 1)[0] || 'NONE')[/:(\d+):in '(.*)'/] || 'NONE'])
  def self.singleton_method_added(m) = ($out << [:sma, m, (caller(1, 1)[0] || 'NONE')[/:(\d+):in '(.*)'/] || 'NONE'])
  def self.const_added(m) = ($out << [:const_added, (caller(1, 1)[0] || 'NONE')[/:(\d+):in '(.*)'/] || 'NONE'])
end
def go
  K.include H
  K.extend H
  K.prepend H
  K.class_eval { def foo; end }
  K.define_method(:bar) {}
  K.const_set(:X, 1)
  def K.sm; end
  K.attr_accessor :zz
  o = Object.new; def o.singleton_method_added(m) = ($out << [:sma2, m, (caller(1, 1)[0] || 'NONE')[/:(\d+):in '(.*)'/] || 'NONE']); def o.q; end
end
go
$out.each { p _1 }
class B; def self.inherited(s); super; p caller(1).grep_v(/Class#initialize/).first(2).map { _1[/:(\d+):in '(.*)'/] }; end; end   # koruby has no Class#initialize frame
def mk = Class.new(B)
[1].each { mk }
class C < B; end

# `raise Klass` with no message: the default message is the qualified class name
module S; class Error < StandardError; end; class NotFound < Error; end; end
p((raise S::NotFound rescue $!.message))
c = Class.new(StandardError)
p c.new.message =~ /\A#<Class:0x\h+>\z/, c.new.inspect =~ /\A#<#<Class:0x\h+>: #<Class:0x\h+>>\z/
# Regexp#to_s escapes a bare `/` like #inspect
p %r{/a/b}.to_s, /x#{%r{/a}}y/
