# Batch 2: conversion via a mock-style #method_missing on a plain object,
# coerce probe through #respond_to?, singleton_method_removed/undefined hooks,
# NoMethodError receiver text, Integer#[] huge index, upto/downto enumerators,
# Hash#each with a lambda, Proc subclass dup/clone, methods(false), extend_object.
class Plain
  def method_missing(name, *args)
    return [1, 2, 3] if name == :to_ary
    super
  end
end
p [Plain.new].flatten
p [1].product(Plain.new)

t = Object.new
$rt = []
def t.respond_to?(m, priv = false)
  $rt << m
  false
end
p(1 <=> t)
p $rt

klass = Class.new
$log = []
def klass.singleton_method_removed(name) = $log << [:removed, name]
def klass.singleton_method_undefined(name) = $log << [:undefined, name]
def klass.to_remove; end
def klass.to_undef; end
class << klass
  remove_method :to_remove
  undef_method :to_undef
end
p $log

o = Object.new
def o.x; end
begin
  o.bar
rescue NoMethodError => e
  puts e.message.sub(/0x\h+/, "0xX")
end
begin
  o.singleton_class.foo
rescue NoMethodError => e
  puts e.message.sub(/0x\h+/, "0xX")
end
begin
  Object.new.bar
rescue NoMethodError => e
  puts e.message
end

big = 2**70
p 3[big], 3[-big], -3[big], -3[big.to_f], 3[big.to_f], 3[-big.to_f], 3[big, 2]
m = Object.new
def m.to_int = 2**70
p 3[m], -1[m]

en = 1.upto("A")
p en.class
begin
  en.size
rescue ArgumentError => e
  puts e.message
end
begin
  en.each { }
rescue ArgumentError => e
  puts e.message
end
p 5.downto(nil).class
begin
  1.upto(nil) { }
rescue ArgumentError => e
  puts e.message
end
p 1.upto(3).size, 3.downto(1).to_a

begin
  { "a" => 1 }.each_pair(&-> k, v { })
rescue ArgumentError => e
  puts "ArgumentError each_pair lambda"
end
r = []
{ "a" => 1 }.each_pair(&-> kv { r << kv })
p r
r = []
{ "a" => 1 }.each(&proc { |k, v| r << [k, v] })
p r
r = []
{ "a" => 1 }.each { |k, v| r << [k, v] }
p r
oo = Object.new
def oo.one(kv) = $one = kv
{ "b" => 2 }.each_pair(&oo.method(:one))
p $one

class MyProc2 < Proc
  def initialize(a, b)
    @first = a
    @second = b
  end
  attr_reader :first, :second, :initializer
  def initialize_copy(other)
    super
    @initializer = :copy
    @first = other.first
    @second = other.second
  end
  def initialize_dup(other)
    super
    @initializer = :dup
  end
  def initialize_clone(other, **options)
    super
    @initializer = :clone
  end
end
obj = MyProc2.new(:a, 2) { }
d = obj.dup
p d.class, d.first, d.second, d.initializer, d.equal?(obj)
c = obj.clone
p c.class, c.initializer

mod = Module.new
def mod.hello; end
p mod.dup.methods(false), mod.methods(false)
s = "str"
def s.solo; end
p s.methods(false)
p Object.new.methods(false)

begin
  Module.instance_method(:extend_object).bind(Class.new).call(Object.new)
rescue TypeError => e
  puts e.message
end
