# Value equality + Array + Array

# Integer equality (hot path, fast)
p 1 == 1                  # true
p 1 == 2                  # false
p 1 != 2                  # true

# String equality
p "abc" == "abc"          # true (value, despite fresh allocs)
p "abc" == "abcd"         # false
p "abc" != "abc"          # false
p "abc" != "abd"          # true

# Mixed types
p 1 == "1"                # false
p [1] == 1                # false

# Array equality (recursive)
p [] == []                # true
p [1, 2, 3] == [1, 2, 3]  # true
p [1, 2] == [1, 2, 3]     # false
p [1, [2, 3]] == [1, [2, 3]]  # true
p [1, [2, 3]] == [1, [2, 4]]  # false

# Array + Array (concat → new)
a = [1, 2, 3]
b = [4, 5]
c = a + b
p c                       # [1, 2, 3, 4, 5]
p a                       # [1, 2, 3] (unchanged)
p b                       # [4, 5]    (unchanged)

# Empty cases
p [] + [1, 2]             # [1, 2]
p [1, 2] + []             # [1, 2]
p [] + []                 # []

# Chain
p [1] + [2] + [3]         # [1, 2, 3]

# Equality after concat
p ([1] + [2, 3]) == [1, 2, 3]  # true
