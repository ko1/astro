module A85
  X = :outer
  class << self
    SM = :sm
    class W
      def initialize = (@v = [SM, X])
      attr_reader :v
    end
    def mk = W.new
  end
end
p A85.mk.v
require "date"
p Date.respond_to?(:new!), DateTime.respond_to?(:new!, true)
class FT < DateTime
  def self.new(y, m, d) = respond_to?(:new!) ? :fast : super
end
p FT.new(2007, 1, 1).to_s
