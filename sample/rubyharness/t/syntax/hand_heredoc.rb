# syntax (hand): heredocs
text = <<~SQUIGGLY
  line one
  line two
SQUIGGLY
p text

dash = <<-DASH
  indented body
  DASH
p dash

raw = <<'RAW'
no #{1 + 1} interpolation here
RAW
p raw

interp = <<"INTERP"
value is #{1 + 2}
INTERP
p interp

a, b = <<~A, <<~B
  first doc
A
  second doc
B
p [a, b]

p(<<~UP.upcase)
  shout me
UP

arr = [<<~ONE, <<~TWO]
  alpha
ONE
  beta
TWO
p arr

joined = <<~X + "tail"
  head
X
p joined
