# `x !~ re` is an intrinsic in CRuby, so the match it performs lands in the
# CALLER's frame: `Regexp.last_match` right after it must see that match.
def probe
  s = "end at 3, 5 is not aligned"
  "zzz" =~ /(z+)/                       # a previous match in this frame
  raise "no" if s !~ /at (\d), (\d)/
  [Regexp.last_match(1), Regexp.last_match(2), $~[0], $1, $2]
end
p probe

def probe2
  "zzz" =~ /(z+)/
  ["abc" !~ /(b)(c)/, $~ && $~[0], $1, $2]
end
p probe2

def probe3
  "zzz" =~ /(z+)/
  [/(b)/ !~ "abc", $1]
end
p probe3

def probe4                              # send / explicit receiver forms
  "zzz" =~ /(z+)/
  ["abc".send(:!~, /(b)/), $1]
end
p probe4

p ["abc" !~ /b/, "abc" !~ /z/, nil !~ /z/]
p Object.new.method(:!~).owner
p [1.respond_to?(:!~), Object.private_method_defined?(:!~)]

class X
  def =~(other) = :custom
end
p (X.new !~ 1)
