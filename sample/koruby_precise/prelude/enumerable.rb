# Enumerable — defined in terms of #each (the only method a host class must
# provide).  Loaded as part of the koruby_precise prelude.
module Enumerable
  # Gather multi-yields like CRuby: `yield 1, 2` → element [1, 2]; a single value
  # stays as-is.  Collection/search use the gathered element; methods that call a
  # user block forward the raw values so block arity destructures them.  For a
  # single-value each, `a.size == 1` so behaviour is unchanged.
  def __each_el; each { |*a| yield(a.size <= 1 ? a[0] : a) }; end
  def map; r = []; each { |*a| r << yield(*a) }; r; end
  def collect; r = []; each { |*a| r << yield(*a) }; r; end
  def select; r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << e if yield(*a) }; r; end
  def filter; r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << e if yield(*a) }; r; end
  def find_all; r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << e if yield(*a) }; r; end
  def reject; r = []; each { |*a| e = a.size <= 1 ? a[0] : a; r << e unless yield(*a) }; r; end
  def flat_map; r = []; each { |*a| v = yield(*a); if v.is_a?(Array); v.each { |e| r << e }; elsif v.respond_to?(:to_ary); ary = v.to_ary; if ary.is_a?(Array); ary.each { |e| r << e }; elsif ary.nil?; r << v; else; raise TypeError, "can't convert #{v.class} to Array (#{v.class}#to_ary gives #{ary.class})"; end; else; r << v; end }; r; end
  def find(ifnone = nil); each { |*a| if yield(*a); return(a.size <= 1 ? a[0] : a); end }; ifnone ? ifnone.call : nil; end
  def detect(ifnone = nil); each { |*a| if yield(*a); return(a.size <= 1 ? a[0] : a); end }; ifnone ? ifnone.call : nil; end
  def to_a; r = []; __each_el { |e| r << e }; r; end
  def to_h(*args); h = {}; bg = block_given?; each(*args) { |*a| pair = bg ? yield(*a) : (a.size <= 1 ? a[0] : a); pair = pair.to_ary if !pair.is_a?(Array) && pair.respond_to?(:to_ary); raise TypeError, "wrong element type #{pair.class} (expected array)" unless pair.is_a?(Array); raise ArgumentError, "element has wrong array length (expected 2, was #{pair.size})" unless pair.size == 2; h[pair[0]] = pair[1] }; h; end
  def entries; r = []; __each_el { |e| r << e }; r; end
  def count(*args); raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 0..1)" if args.size > 1; n = 0; if args.size > 0; item = args[0]; __each_el { |x| n += 1 if x == item }; elsif block_given?; each { |*a| n += 1 if yield(*a) }; else; each { |*a| n += 1 }; end; n; end
  def include?(v); __each_el { |e| return true if e == v }; false; end
  def member?(v); __each_el { |e| return true if e == v }; false; end
  def first(n = nil); if n.nil?; __each_el { |e| return e }; nil; else; r = []; c = 0; __each_el { |e| if c < n; r << e; c += 1; end }; r; end; end
  # reduce/inject: (sym) | (init, sym) | { block } | (init) { block }.
  def reduce(*args); if block_given?; if args.size >= 1; acc = args[0]; f = false; else; acc = nil; f = true; end; __each_el { |x| if f; acc = x; f = false; else; acc = yield(acc, x); end }; acc; else; if args.size >= 2; acc = args[0]; op = args[1]; f = false; else; op = args[0]; acc = nil; f = true; end; __each_el { |x| if f; acc = x; f = false; else; acc = acc.send(op, x); end }; acc; end; end
  def inject(*args, &blk); reduce(*args, &blk); end
  def sum(init = 0); s = init; if block_given?; each { |*a| s = s + yield(*a) }; else; __each_el { |x| s = s + x }; end; s; end
  # min/max: (), (n), { cmp }, (n) { cmp }.  n → the n smallest/largest as an Array.
  def min(n = nil, &blk); s = blk ? to_a.sort(&blk) : to_a.sort; n ? s.first(n) : s.first; end
  def max(n = nil, &blk); s = blk ? to_a.sort(&blk) : to_a.sort; n ? s.last(n).reverse : s.last; end
  def min_by(n = nil); s = sort_by { |x| yield(x) }; n ? s.first(n) : s.first; end
  def max_by(n = nil); s = sort_by { |x| yield(x) }; n ? s.last(n).reverse : s.last; end
  def sort; to_a.sort; end
  def sort_by; a = []; __each_el { |x| a << x }; a.sort_by { |x| yield(x) }; end
  # all?/any?/none?/one? accept an optional pattern (uses pattern === x), else a block, else truthiness.
  def all?(*a); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; if a.size > 0; pt = a[0]; __each_el { |x| return false unless pt === x }; elsif block_given?; each { |*ar| return false unless yield(*ar) }; else; __each_el { |x| return false unless x }; end; true; end
  def any?(*a); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; if a.size > 0; pt = a[0]; __each_el { |x| return true if pt === x }; elsif block_given?; each { |*ar| return true if yield(*ar) }; else; __each_el { |x| return true if x }; end; false; end
  def none?(*a); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; if a.size > 0; pt = a[0]; __each_el { |x| return false if pt === x }; elsif block_given?; each { |*ar| return false if yield(*ar) }; else; __each_el { |x| return false if x }; end; true; end
  def one?(*a); raise ArgumentError, "wrong number of arguments (given #{a.size}, expected 0..1)" if a.size > 1; n = 0; if a.size > 0; pt = a[0]; __each_el { |x| n += 1 if pt === x }; elsif block_given?; each { |*ar| n += 1 if yield(*ar) }; else; __each_el { |x| n += 1 if x }; end; n == 1; end
  def cycle(n = nil); a = to_a; return nil if a.empty?; if n.nil?; loop { a.each { |x| yield x } }; else; n.times { a.each { |x| yield x } }; end; nil; end
  def each_with_object(o); __each_el { |x| yield x, o }; o; end
  def each_with_index; i = 0; __each_el { |x| yield x, i; i += 1 }; self; end
  def partition; a = []; b = []; each { |*ar| e = ar.size <= 1 ? ar[0] : ar; if yield(*ar); a << e; else; b << e; end }; [a, b]; end
  def group_by; h = {}; __each_el { |x| k = yield(x); h[k] = [] unless h.key?(k); h[k] << x }; h; end
  def tally(h = {}); __each_el { |x| h[x] = (h[x] || 0) + 1 }; h; end
  def chunk; r = []; lastk = nil; f = true; __each_el { |x| k = yield(x); if k.nil? || k == :_separator; f = true; next; end; if f || k != lastk; r << [k, [x]]; f = false; else; r.last[1] << x; end; lastk = k }; r; end
  def chunk_while; r = []; cur = nil; f = true; prev = nil; __each_el { |x| if f; cur = [x]; f = false; elsif yield(prev, x); cur << x; else; r << cur; cur = [x]; end; prev = x }; r << cur unless cur.nil?; r; end
  def slice_when; r = []; cur = nil; f = true; prev = nil; __each_el { |x| if f; cur = [x]; f = false; elsif yield(prev, x); r << cur; cur = [x]; else; cur << x; end; prev = x }; r << cur unless cur.nil?; r; end
  def take(n); n = n.to_int unless n.is_a?(Integer); raise ArgumentError, "attempt to take negative size" if n < 0; r = []; __each_el { |x| break if r.size >= n; r << x }; r; end
  def drop(n); n = n.to_int unless n.is_a?(Integer); raise ArgumentError, "attempt to drop negative size" if n < 0; r = []; i = 0; __each_el { |x| r << x if i >= n; i += 1 }; r; end
  def take_while; r = []; each { |*ar| e = ar.size <= 1 ? ar[0] : ar; break unless yield(*ar); r << e }; r; end
  def drop_while; r = []; dropping = true; each { |*ar| e = ar.size <= 1 ? ar[0] : ar; dropping = false if dropping && !yield(*ar); r << e unless dropping }; r; end
  def minmax; [min, max]; end
  def minmax_by; [min_by { |x| yield(x) }, max_by { |x| yield(x) }]; end
  def find_index(v = nil); i = 0; if block_given?; each { |*ar| return i if yield(*ar); i += 1 }; else; __each_el { |x| return i if x == v; i += 1 }; end; nil; end
  def each_slice(n); n = n.to_int unless n.is_a?(Integer); raise ArgumentError, "invalid slice size" unless n > 0; r = []; s = []; __each_el { |x| s << x; if s.size == n; r << s; s = []; end }; r << s unless s.empty?; if block_given?; r.each { |sl| yield sl }; nil; else; r.each; end; end
  def each_cons(n); n = n.to_int unless n.is_a?(Integer); raise ArgumentError, "invalid size" unless n > 0; r = []; buf = []; __each_el { |x| buf << x; if buf.size == n; r << buf.dup; buf.shift; end }; if block_given?; r.each { |cc| yield cc }; nil; else; r.each; end; end
  def zip(*others); os = others.map { |o| o.to_a }; r = []; i = 0; __each_el { |x| row = [x]; os.each { |o| row << o[i] }; r << row; i += 1 }; if block_given?; r.each { |row| yield row }; nil; else; r; end; end
  def filter_map; r = []; each { |*ar| v = yield(*ar); r << v if v }; r; end
  def collect_concat(&blk); flat_map(&blk); end
  def reverse_each; a = to_a.reverse; return a.each unless block_given?; a.each { |x| yield x }; self; end
  def uniq; seen = {}; r = []; __each_el { |x| k = block_given? ? yield(x) : x; unless seen.key?(k); seen[k] = true; r << x; end }; r; end
  def each_entry; __each_el { |x| yield x }; self; end
end
