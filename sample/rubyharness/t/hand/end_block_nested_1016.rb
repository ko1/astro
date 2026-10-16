# END { } body: blocks inside it, pattern / multiple / rescue binds of the enclosing locals (2026-10-16)
x = 1
END {
  [1].each { |q| p [x, q] }
  y = x + 1
  [2].each { p y }
  case [x]
  in [z] then p z
  end
  a, b = x, 2
  p [a, b]
  begin; raise "e"; rescue => er; p er.message; end
  [3].each { |w| y += w }
  p y
}
