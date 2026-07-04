# Integer#round/#truncate digits arg: #to_int coercion, RangeError for a
# non-finite Float / Bignum count, TypeError for a non-numeric. vs ruby.
class ToI; def to_int; -1; end; end
p 123.round(ToI.new)
p 12345.round(-2)
p 12345.round(-3)
p 5.round
p 123.round(1)
p 99.truncate(-1)
begin; 1.round(Float::INFINITY); rescue RangeError; p :inf; end
begin; 1.round(-Float::INFINITY); rescue RangeError; p :ninf; end
begin; 1.round(2**70); rescue RangeError; p :big; end
begin; 1.round("x"); rescue TypeError; p :type; end
