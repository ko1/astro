# #parameters reports anonymous * / ** as names :* / :** (Ruby 3.x). vs ruby.
def m1(*); end;            p method(:m1).parameters
def m2(**); end;           p method(:m2).parameters
def m6(a, *rest, **k); end; p method(:m6).parameters
def m7(a, *args); end;     p method(:m7).parameters
def m8(a, b=1, *c, d, **e); end; p method(:m8).parameters
p proc { |a, *r| }.parameters
p proc { |a, **k| }.parameters
p lambda { |a, *r, **k| }.parameters
