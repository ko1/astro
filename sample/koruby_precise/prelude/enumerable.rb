# Enumerable — defined in terms of #each (the only method a host class must
# provide).  Loaded as part of the koruby_precise prelude.
module Enumerable
  def map; r = []; each { |x| r << yield(x) }; r; end
  def collect; r = []; each { |x| r << yield(x) }; r; end
  def select; r = []; each { |x| r << x if yield(x) }; r; end
  def filter; r = []; each { |x| r << x if yield(x) }; r; end
  def find_all; r = []; each { |x| r << x if yield(x) }; r; end
  def reject; r = []; each { |x| r << x unless yield(x) }; r; end
  def flat_map; r = []; each { |x| v = yield(x); if v.is_a?(Array); v.each { |e| r << e }; else; r << v; end }; r; end
  def find(ifnone = nil); each { |x| return x if yield(x) }; ifnone ? ifnone.call : nil; end
  def detect(ifnone = nil); each { |x| return x if yield(x) }; ifnone ? ifnone.call : nil; end
  def to_a; r = []; each { |x| r << x }; r; end
  def to_h; h = {}; if block_given?; each { |x| kv = yield(x); h[kv[0]] = kv[1] }; else; each { |x| h[x[0]] = x[1] }; end; h; end
  def entries; r = []; each { |x| r << x }; r; end
  def count; n = 0; each { |x| n += 1 }; n; end
  def include?(v); each { |x| return true if x == v }; false; end
  def member?(v); each { |x| return true if x == v }; false; end
  def first(n = nil); if n.nil?; each { |x| return x }; nil; else; r = []; c = 0; each { |x| if c < n; r << x; c += 1; end }; r; end; end
  def reduce(a, b = nil); if b.nil?; acc = nil; f = true; each { |x| if f; acc = x; f = false; else; acc = acc.send(a, x); end }; acc; else; acc = a; each { |x| acc = acc.send(b, x) }; acc; end; end
  def inject(a, b = nil); if b.nil?; acc = nil; f = true; each { |x| if f; acc = x; f = false; else; acc = acc.send(a, x); end }; acc; else; acc = a; each { |x| acc = acc.send(b, x) }; acc; end; end
  def sum(init = 0); s = init; if block_given?; each { |x| s = s + yield(x) }; else; each { |x| s = s + x }; end; s; end
  def min; r = nil; f = true; each { |x| if f; r = x; f = false; elsif x < r; r = x; end }; r; end
  def max; r = nil; f = true; each { |x| if f; r = x; f = false; elsif x > r; r = x; end }; r; end
  def min_by; r = nil; rk = nil; f = true; each { |x| k = yield(x); if f; r = x; rk = k; f = false; elsif k < rk; r = x; rk = k; end }; r; end
  def max_by; r = nil; rk = nil; f = true; each { |x| k = yield(x); if f; r = x; rk = k; f = false; elsif k > rk; r = x; rk = k; end }; r; end
  def sort; to_a.sort; end
  def sort_by; a = []; each { |x| a << x }; a.sort_by { |x| yield(x) }; end
  def all?; bg = block_given?; each { |x| return false unless (bg ? yield(x) : x) }; true; end
  def any?; bg = block_given?; each { |x| return true if (bg ? yield(x) : x) }; false; end
  def none?; bg = block_given?; each { |x| return false if (bg ? yield(x) : x) }; true; end
  def one?; n = 0; bg = block_given?; each { |x| n += 1 if (bg ? yield(x) : x) }; n == 1; end
  def cycle(n = nil); a = to_a; return nil if a.empty?; if n.nil?; loop { a.each { |x| yield x } }; else; n.times { a.each { |x| yield x } }; end; nil; end
  def each_with_object(o); each { |x| yield x, o }; o; end
  def each_with_index; i = 0; each { |x| yield x, i; i += 1 }; self; end
  def partition; a = []; b = []; each { |x| if yield(x); a << x; else; b << x; end }; [a, b]; end
  def group_by; h = {}; each { |x| k = yield(x); h[k] = [] unless h.key?(k); h[k] << x }; h; end
  def tally; h = {}; each { |x| h[x] = (h[x] || 0) + 1 }; h; end
  def chunk; r = []; lastk = nil; f = true; each { |x| k = yield(x); if f || k != lastk; r << [k, [x]]; f = false; else; r.last[1] << x; end; lastk = k }; r; end
  def chunk_while; r = []; cur = nil; f = true; prev = nil; each { |x| if f; cur = [x]; f = false; elsif yield(prev, x); cur << x; else; r << cur; cur = [x]; end; prev = x }; r << cur unless cur.nil?; r; end
  def slice_when; r = []; cur = nil; f = true; prev = nil; each { |x| if f; cur = [x]; f = false; elsif yield(prev, x); r << cur; cur = [x]; else; cur << x; end; prev = x }; r << cur unless cur.nil?; r; end
  def take(n); r = []; each { |x| break if r.size >= n; r << x }; r; end
  def drop(n); r = []; i = 0; each { |x| r << x if i >= n; i += 1 }; r; end
  def take_while; r = []; each { |x| break unless yield(x); r << x }; r; end
  def drop_while; r = []; dropping = true; each { |x| dropping = false if dropping && !yield(x); r << x unless dropping }; r; end
  def minmax; [min, max]; end
  def minmax_by; [min_by { |x| yield(x) }, max_by { |x| yield(x) }]; end
  def find_index(v = nil); i = 0; bg = block_given?; each { |x| return i if (bg ? yield(x) : (x == v)); i += 1 }; nil; end
  def each_slice(n); n = n.to_int unless n.is_a?(Integer); raise ArgumentError, "invalid slice size" unless n > 0; r = []; s = []; each { |x| s << x; if s.size == n; r << s; s = []; end }; r << s unless s.empty?; if block_given?; r.each { |sl| yield sl }; nil; else; r; end; end
  def each_cons(n); n = n.to_int unless n.is_a?(Integer); raise ArgumentError, "invalid size" unless n > 0; r = []; buf = []; each { |x| buf << x; if buf.size == n; r << buf.dup; buf.shift; end }; if block_given?; r.each { |cc| yield cc }; nil; else; r; end; end
  def zip(*others); os = others.map { |o| o.to_a }; r = []; i = 0; each { |x| row = [x]; os.each { |o| row << o[i] }; r << row; i += 1 }; if block_given?; r.each { |row| yield row }; nil; else; r; end; end
  def filter_map; r = []; each { |x| v = yield(x); r << v if v }; r; end
  def collect_concat; r = []; each { |x| v = yield(x); if v.is_a?(Array); v.each { |e| r << e }; else; r << v; end }; r; end
  def reverse_each; a = to_a.reverse; return a.each unless block_given?; a.each { |x| yield x }; self; end
  def uniq; seen = {}; r = []; each { |x| k = block_given? ? yield(x) : x; unless seen.key?(k); seen[k] = true; r << x; end }; r; end
  def each_entry; each { |x| yield x }; self; end
end
