# `recv.m(args, &block)` where `block` happens to be nil took a path that never
# staged the frame link, so the backtrace (and `caller`) stopped at the callee.
# rspec's `ExpectationTarget#to` forwards its (usually absent) block that way,
# which cut every failure backtrace off above the matcher.
module H
  def self.deep = caller(0, 8)
  def self.handle(a, b = nil, &blk) = deep
end
module T
  def to(m = nil, n = nil, &block) = H.handle(m, n, &block)
end
class Target; include T; end
def outer1 = Target.new.to(:m)
def outer2 = Target.new.to(:m) { }
puts outer1
puts "---"
puts outer2
puts "---"

# the same shapes with a splat, and with an implicit-self receiver
module H2
  def self.deep = caller(0, 6)
  def self.handle(*a, &blk) = deep
end
def spl(*a, &block) = H2.handle(*a, &block)
puts spl(1, 2)
puts "---"
def imp(&block) = h3(&block)
def h3(&blk) = caller(0, 5)
puts imp
puts "---"

# a raise carries the same link
def r_inner(&blk) = raise("boom")
def r_outer(&block) = Target.new.to(:x) { }
def r_mid(&block) = r_inner(&block)
begin
  r_mid
rescue => e
  puts e.backtrace.first(3)
end
