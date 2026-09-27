# A block written in a method of a file with `using` keeps its refinements when
# a method from another (unrefined) file yields it; a block from the unrefined
# file does not gain them when a refined method yields it.
require "tmpdir"
dir = Dir.mktmpdir("refblk")
File.write("#{dir}/plain.rb", <<~'RUBY')
  def rb_yielder(x) = yield(x)
  def rb_each2(a) = a.each { |e| yield e }
  def rb_plain_block_to(o) = o.run { |x| x.rbdc rescue :unrefined }
RUBY
require "#{dir}/plain"
module RBR; refine Array do; def rbdc = :refined; end; end
using RBR
class RBC
  def a = rb_yielder([1]) { |x| x.rbdc }
  def b = [[1]].each_with_index.map { |x, i| x.rbdc }
  def c = rb_each2([[1], [2]]) { |x| p x.rbdc }
  def d = [[1]].map { |x| rb_yielder(x) { |y| y.rbdc } }
  def run = yield([1])
  def e = 3.times.map { a }
end
o = RBC.new
p o.a, o.b, o.d, o.e; o.c
p rb_plain_block_to(o)
p o.a
require "fileutils"; FileUtils.rm_rf(dir)
