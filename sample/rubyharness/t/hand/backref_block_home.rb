# `$~` in a block is the block's home method's, even when the block is called
# from inside another method that has its own `$~` (radius: `r($1) { r($2) }`).

def r(name, &blk)
  if name =~ /^(.+?):(.+)$/
    r($1) { r($2, &blk) }
  else
    "<#{name}#{blk ? "[" + blk.call + "]" : ""}>"
  end
end
p r("a:b:c:d")

def own_match
  "q" =~ /(q)/
  yield
end
def reads_home
  "xy" =~ /(x)(y)/
  own_match { $2 }
end
p reads_home

# a match inside the block lands in the home frame, and the yielder keeps its own
def yielder_keeps
  "k" =~ /(k)/
  yield
  $1
end
def writes_home
  "ab" =~ /(a)(b)/
  inner = yielder_keeps { "zz" =~ /(z)/ }
  [inner, $1, $~[0]]
end
p writes_home

# gsub's block sees gsub's match (a C method: nothing in between)
def via_gsub = "a1b2".gsub(/([a-z])(\d)/) { "#{$2}#{$1}" }
p via_gsub

# nested blocks
def nest
  "mn" =~ /(m)(n)/
  own_match { own_match { $2 } }
end
p nest

# raise out of the block still puts things back
def raises_in_block
  "ef" =~ /(e)(f)/
  begin
    own_match { raise "x" if $2 }
  rescue
    [$1, $2]
  end
end
p raises_in_block

# a Fiber / Thread body is a block of the method that made it
def in_fiber
  "gh" =~ /(g)(h)/
  Fiber.new { $2 }.resume
end
p in_fiber
def in_thread
  "ij" =~ /(i)(j)/
  Thread.new { $2 }.value
end
p in_thread

# a block inside such a block (gsub under a yield) sees gsub's match (nickel)
def ns(s, re)
  m = s.match(re)
  yield(*m.captures)
end
def st
  "zz" =~ /(z)/
  ns("jan 2nd 3rd", /(jan) (.*)/) do |m1, m2|
    m2.gsub(/(\d)(nd|rd)/) { m1 + "/" + Regexp.last_match(1) + $2 }
  end
end
p st
