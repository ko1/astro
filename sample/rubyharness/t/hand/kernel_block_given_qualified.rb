# `Kernel.block_given?` / `::Kernel::block_given?` は裸の block_given? と同じ意味。
# builder がその書き方をする。
def a; Kernel.block_given?; end
def b; ::Kernel::block_given?; end
def c2; block_given?; end
def d; [1].map { Kernel.block_given? }; end
p a, a { }, b, b { }, c2, c2 { }, d, d { }
