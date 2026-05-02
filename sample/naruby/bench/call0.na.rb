def zero(n) = one(n)
def one(n) = 1

def test
  i=0
  while i<1_000_000_000
    zero 0
    i+=1
  end
end

test
p zero(0)

=begin
                      user     system      total        real
ruby              0.000145   0.000073  34.384568 ( 34.404351)
ruby/yjit         0.000361   0.000181  24.010389 ( 23.981052)
naruby/interpret  0.000250   0.000125  20.254094 ( 20.253549)
naruby/compiled   0.000127   0.000063   3.520829 (  3.510728)
naruby/pg         0.000137   0.000068   1.164021 (  1.163381)
=end

