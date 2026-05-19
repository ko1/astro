# string_concat_dyn — dynamic string concatenation that defeats parser
# constant folding.  Stresses the actual string allocator at runtime.
#
# string_concat.ba.rb (iter 37) became 50%+ faster after parser-time
# const-fold of literal string + literal string was added.  That's a
# legitimate optimization, but it makes string_concat measure "1 alloc
# per iter" instead of "5 allocs per iter".  This bench keeps the
# "many small string allocs" pattern by using variables.

def make_chunk(i)
  # Different bytes per iteration so the parser can't fold this
  # expression at compile time.
  if i % 3 == 0
    "aaa"
  elsif i % 3 == 1
    "bbb"
  else
    "ccc"
  end
end

def run(iters)
  i = 0
  total = 0
  while i < iters
    a = make_chunk(i)
    b = make_chunk(i + 1)
    c = make_chunk(i + 2)
    s = a + b + c
    total = total + s.size
    i = i + 1
  end
  total
end

p run(5_000_000)
