wm = ObjectSpace::WeakMap.new
k = Object.new; v = "val"
wm[k] = v; wm[1] = :imm
p wm[k], wm.key?(k), wm.size, wm[1], wm.include?(Object.new)
def put(wm, k) = (wm[k] = Object.new; nil)
k2 = Object.new; put(wm, k2)
def junk(n) = n.zero? ? nil : (Array.new(100) { Object.new }; junk(n - 1))
5.times { junk(50); GC.start }
p wm.key?(k2), wm[k] == v, wm.keys.size, wm.values.size
p wm.delete(k), wm.key?(k), wm.delete(:nope) { |x| [:none, x] }
wm.each { |a, b| p [a, b] }
p wm.inspect.gsub(/0x\h+/, "X")
require "weakref"
def mk(l = 10) = l > 0 ? mk(l - 1) : WeakRef.new(Object.new)
ws = []; w = nil
100.times { ws << mk; GC.start; GC.start; break if (w = ws.find { !_1.weakref_alive? }) }
p w.nil?
