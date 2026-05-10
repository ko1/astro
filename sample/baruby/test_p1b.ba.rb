# <=>, *, <<, escape

# <=> spaceship
p 1 <=> 2          # -1
p 1 <=> 1          # 0
p 2 <=> 1          # 1
p "abc" <=> "abd"  # -1
p "abc" <=> "abc"  # 0
p "abd" <=> "abc"  # 1
p 1 <=> "1"        # nil
p [] <=> []        # nil  (no Array <=> yet)

# Integer << shift
p 1 << 3           # 8
p 256 << 4         # 4096

# Array << push (mutating)
a = [1, 2]
a << 3
a << 4
p a                # [1, 2, 3, 4]

# String << append (mutating)
s = "hello"
s << " "
s << "world"
p s                # "hello world"

# String * repeat
p "ab" * 3         # "ababab"
p "x" * 0          # ""
p "x" * -1         # ""

# Array * repeat
p [1, 2] * 3       # [1, 2, 1, 2, 1, 2]
p [] * 5           # []
p [9] * 0          # []

# Escape sequences via prism
p "a\nb"           # "a\nb" (with embedded newline → 3 chars)
p "tab\there"      # "tab\there"
p "back\\slash"    # "back\\slash"
p "quote\""        # "quote\""
p "a\nb".size      # 3

# Mixed: << with chain
xs = [10]
xs << 20 << 30 << 40
p xs               # [10, 20, 30, 40]
