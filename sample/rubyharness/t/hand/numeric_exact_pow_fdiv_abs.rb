# Complex#** keeps exact components on an axis / diagonal and uses CRuby's polar
# form for real exponents; 0/1 ** Rational stay exact; Rational#fdiv divides exactly
# first; Complex#abs keeps an exact zero-part magnitude; Math.lgamma's sign;
# Numeric#i (Rational).
p [(1/2r).i, 1.5.i, 3.i, (Complex(1,2).i rescue $!.class), Complex.method_defined?(:i)]
[[0, 1/2r], [4, 1/2r], [1, 1/3r], [0, 2r], [2, 1/2r], [1/4r, 1/2r], [0, -1/2r], [1/2r, 1/2r], [0r, 1/2r], [-8, 1/3r], [2**70, 1/2r], [0, 0.5]].each { |a, b| p [a, b, (a ** b rescue $!.class)] }
zs = [Complex(2,0), Complex(0,2), Complex(1,1), Complex(1,-1), Complex(-2,0), Complex(0,-3), Complex(-1,-1), Complex(1/2r,0), Complex(2.0,0), Complex(0,1.5), Complex(1,2), Complex(0,0), Complex(3,4), Complex(-1/2r, 1/2r)]
es = [-3, -2, -1, 0, 1, 2, 3, 5, 10, 2r, 1/2r, 0.5, Complex(2, 0), 2**70]
zs.each { |z| p [z, es.map { |e| (z ** e rescue $!.class) }] }
p (0+0i)**10, (0+0i)**0, (0+0i)**1, (2+0i)**3, (2+0i)**10, (1/2r+0i)**-1, (1+1i)**-2, Complex(0.0,0)**2, (0+0i)**-1 rescue p $!
p (2+0i)**0.5, (1+2i)**(1/2r), (0+0i)**(0.0), Complex(1,2)**Complex(0,1), (-8+0i)**(1/3r)
p [((0+0i)**-2 rescue $!.class)], (3+4i)**2, (1r+2ri)**3
[Complex(2,0), Complex(0,-2), Complex(2.0,0), Complex(2,0.0), Complex(0,0), Complex(0.0,0), Complex(0,0.0), Complex(3,4), Complex(1/2r,0), Complex(0,-(2**70)), Complex(-0.0, 3)].each { |z| p [z, z.abs, z.polar, z.magnitude] }
p (0/1r).fdiv(3), (5/3r).fdiv(-1), (5/3r).fdiv(5), (1/3r).fdiv(1/3r), (1/3r).fdiv(0.5), (-0/1r).fdiv(-3), (1/3r).fdiv(0), (1/3r).fdiv(0.0), (-1/3r).fdiv(0), (1/3r).fdiv(2**70)
p [((1/3r).fdiv(Complex(1, 1)) rescue $!.class)]
p [((1/3r).fdiv("x") rescue [$!.class, $!.message])]
p [((1/3r).fdiv(nil) rescue [$!.class, $!.message])]
[-0.0, 0.0, -1.0, -2.0, -2.5, Float::INFINITY, -Float::INFINITY, Float::NAN].each { |x| p [x, (Math.lgamma(x) rescue $!.class)] }
