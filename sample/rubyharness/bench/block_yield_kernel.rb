# block-yield microbench — isolates korb_block_yield (times/each simple blocks),
# the #2 hot runtime function in the DOOM AOT profile (render loops).
N    = 4000
ROWS = 1200
arr  = Array.new(N) { |i| i }

acc = 0
r = 0
while r < ROWS
  # 1-arg block over an Array (each) — the common render iteration shape
  arr.each { |x| acc = (acc + x * 3) & 0x3fff_ffff }
  # 0-arg-ish integer times with the index
  N.times { |i| acc = (acc ^ (i + r)) & 0x3fff_ffff }
  r += 1
end
p acc
