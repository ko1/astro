# `def self.new(*a, &b) = super` から Class#new に入るときもブロックは届く
# (concurrent-ruby の SafeInitialization が block_given? で検査する)。
class BaseN
  def initialize(*a, &b) = nil
end
module SafeInit
  def new(*args, &block) = super(*args, &block)
end
class SubN < BaseN
  class << self; prepend SafeInit; end
  def initialize(o = {}, &block) = p([block_given?, !block.nil?, block&.call])
end
SubN.new(1) { :x }
SubN.new(1)
class SubN2 < BaseN
  def self.new(*args, &block) = super
  def initialize(o = {}, &block) = p([block_given?, !block.nil?])
end
SubN2.new(1) { :x }
class SubN3
  def self.new(*args, &block) = super(*args, &block)
  def initialize(&block) = p([block_given?, block&.call])
end
SubN3.new { :y }
