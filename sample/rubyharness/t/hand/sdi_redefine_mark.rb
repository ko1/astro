# An AOT inline send (empty? / size / nil? / class) stands for the builtin only while the cached entry is it:
# a Ruby redefinition reusing the entry, an alias onto another builtin and a subclass override all dispatch
class SubS < String; def empty? = :sub; end
def probe(s, a, o) = [s.empty?, a.size, o.nil?, s.class]
r = []
3.times { r << probe("", [1, 2], nil) }
r << probe(SubS.new(""), [], 1)
class String; def empty? = :redef; end
3.times { r << probe("x", [1], nil) }
class Array; alias_method :size, :first; end
3.times { r << probe("", [7, 8], nil) }
class NilClass; def nil? = :no; end
r << probe("", [], nil)
p r
