# Proc#arity: required = leading + post; a rest or lambda-optional negates;
# a plain proc's optionals/keywords don't. vs ruby.
p ->(*a, b) {}.arity
p ->(a, *b, c) {}.arity
p ->(a, b=1, c=2, *d, e, f) {}.arity
p lambda { |a, b=1, c=2, *d, e, f| }.arity
p proc { |a=1| }.arity
p proc { |a=1, b: 2| }.arity
p proc { |a, b=1| }.arity
p proc { |*a, b| }.arity
p ->(a, b) {}.arity
p ->() {}.arity
p ->(a, b=1) {}.arity
p proc { |a, b| }.arity
p ->(a, k:) {}.arity
p ->(a, k: 1) {}.arity
p ->(a:, b:) {}.arity
p ->(**k) {}.arity
p ->(a, **k) {}.arity
