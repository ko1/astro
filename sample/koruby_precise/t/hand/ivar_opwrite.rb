class C
  def initialize; @n = 100; @x = nil; @m = 7; end
  def run; @n /= 4; @n += 3; @n *= 2; @x ||= 5; @x ||= 9; @m &&= 1; [@n, @x, @m]; end
end
p C.new.run
