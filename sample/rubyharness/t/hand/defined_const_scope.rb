# `defined?(Const)` resolves like a bare constant READ: lexical scope, its
# ancestry, then top level.  The name table is flat, so a by-name probe used to
# answer for a constant in any namespace at all.
require "net/http"
p [defined?(HTTP), defined?(HTTPOK), defined?(Net::HTTP), defined?(Net::Nope)]

module A1
  K = 1
  private_constant :K
  def self.from_self = defined?(K)
  module N
    def self.from_scope = defined?(K)
    def self.from_block = [1].map { defined?(K) }.first
  end
  class Inst
    def m = defined?(K)
    def nope = defined?(Missing)
  end
end
module A2                      # same nested name, different namespace
  K2 = 1
  module N
    def self.from_scope = defined?(K2)
    def self.leaks = defined?(K)   # A1::K is NOT visible here
  end
end
class A3
  K3 = 1
  class << self
    Q = 2
    def in_sclass = [defined?(K3), defined?(Q)]
  end
end

p [A1.from_self, A1::N.from_scope, A1::N.from_block, A1::Inst.new.m, A1::Inst.new.nope]
p [A2::N.from_scope, A2::N.leaks]
p A3.in_sclass

module Wrap
  INNER = 1
end
p [defined?(INNER), defined?(Wrap::INNER), defined?(Wrap::NOPE)]

# a constant whose value is nil still counts; an autoload counts before loading
NILC = nil
p defined?(NILC)
module AL
  autoload :Pending, "nonexistent_file_for_autoload"
  def self.probe = defined?(Pending)
end
p AL.probe
p defined?(AL::Pending)
