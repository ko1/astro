# Fine-grained substring allocation.  A retained "text" String is read
# at every offset, producing a fresh 5-char substring each time.  Tests
# the path that allocates many small short-lived BaStrings while one
# large BaString stays live.

def repeat(s, n)
  s * n
end

text = repeat("abcdef", 3_000_000) # 18_000_000 byte String, lives the whole run
n    = text.size - 5

i   = 0
sum = 0
while i < n
  word = text[i, 5]
  sum  = sum + word.size
  i    = i + 1
end
p sum
