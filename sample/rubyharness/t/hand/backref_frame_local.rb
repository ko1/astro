# `$~` (and $1.., $&, $`, $') belongs to the METHOD frame: a callee's match must
# not overwrite the caller's.  Blocks share their enclosing method's.
def inner
  "zzz" =~ /(z+)/
  $~[1]
end
def outer
  "abc" =~ /(b)(c)/
  a = Regexp.last_match(2)
  inner
  b = Regexp.last_match(1)
  [a, b, $~ && $~[0], $1, $2]
end
p outer

def blk
  "abc" =~ /(b)/
  [1].each { "xyz" =~ /(y)/ }     # same method → shared
  [$~[1], $1]
end
p blk

def fresh = [$~, $1]              # a fresh frame starts at nil
"abc" =~ /(b)/
p fresh
p [$~[1], $1]

def setter
  $~ = nil                        # frame-local too
  :ok
end
"abc" =~ /(b)/
p [setter, $1]

def via_match(s)
  s.match(/(x)/)
  :done
end
"abc" =~ /(b)/
p [via_match("xyz"), $1]

def via_gsub(s) = s.gsub(/x/) { "y" }
"abc" =~ /(b)/
p [via_gsub("xx"), $1]

def via_scan(s)
  s.scan(/(q)/)
  :scanned
end
"abc" =~ /(b)/
p [via_scan("qq"), $1]

# a builtin called directly DOES set the caller's (CRuby: C frames have no svar)
"abc" =~ /(b)/
"zz".sub(/z/) { "" }
p $1

# the pry shape: a match, then a call, then $'
def postmatch(line)
  if line =~ /\Adef (?:.*?\.)?test(?=[\(\s;]|$)/
    "def #{helper}#{$'}"
  end
end
def helper
  "n" =~ /n/
  "test"
end
p postmatch("def x.test; :after; end\n")
