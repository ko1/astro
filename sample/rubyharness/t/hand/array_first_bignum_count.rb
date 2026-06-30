# Array#first/last/take/drop with a Bignum count raise RangeError. vs ruby.
a = [1, 2, 3]
begin; a.first(10**20); rescue => e; p e.class; end
begin; a.last(10**20); rescue => e; p e.class; end
begin; a.take(10**20); rescue => e; p e.class; end
begin; a.drop(10**20); rescue => e; p e.class; end
p a.first(2); p a.last(2); p a.take(2); p a.drop(2)
p a.first(0); p a.first(99)
