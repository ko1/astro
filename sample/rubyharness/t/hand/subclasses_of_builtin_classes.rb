# Class#subclasses also lists the classes made in C (Exception's, Numeric's, ...),
# filed on first use; user subclasses keep showing up after that.
p Exception.subclasses.map(&:to_s).sort
p StandardError.subclasses.map(&:to_s).sort.first(8)
p Integer.subclasses, Numeric.subclasses.map(&:to_s).sort, IOError.subclasses.sort_by(&:to_s)
class Foo; end; class Bar < Foo; end; class Baz < Foo; end
p Foo.subclasses.sort_by(&:name); n0 = String.subclasses.size   # boot-time internals differ (CRuby: Warning::buffer)
class MyStr < String; end
p String.subclasses.include?(MyStr), String.subclasses.size - n0, Class.new(Foo).then { Foo.subclasses.size }
p Object.subclasses.include?(String), Object.subclasses.include?(Comparable), Object.subclasses.any?(&:singleton_class?)
