# Klass[...] and class-level attr readers through the site cache: user `def self.[]`, builtin Array[]/Hash[]/Set[] and their subclasses
class K; def self.[](*a) = [:k, a]; class << self; attr_accessor :conf; end; end
class Hd < Hash; def self.[](h) = new.merge!(h.transform_keys(&:to_s)); end
class MyA < Array; end
class MyH < Hash; end
K.conf = 1
2.times do |i|
  p K[1], K[1, 2], K.conf, Hd[a: 1], Hd[a: 1].class, Array[1, 2], Hash[[[:x, 1]]], Set[1, 2].size
  p MyA[3, 4], MyA[3, 4].class, MyH[:q, 1], MyH[:q, 1].class
  K.conf = i + 10
end
p K.conf
class K; def self.[](x) = [:redef, x]; end
p K[7]
class K; class << self; def conf = :redefined_reader; end; end
p K.conf
class MyA; def self.[](*) = :sub_redef; end
p MyA[1], Array[1]
3.times { |i| GC.start if i == 1; p K[i], Hd[b: i] }
begin
  Object[1]
rescue NoMethodError => e
  p e.message[0, 30]
end
