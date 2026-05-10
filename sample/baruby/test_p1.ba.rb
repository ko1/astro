# P1 language extensions

# true / false / nil literals
p true
p false
p nil

# nil / false distinction
p nil == false       # false (Ruby semantics)
p nil == nil         # true
p false == false     # true

# truthy/falsy in if
if nil
  p "nil truthy?"
else
  p "nil falsy"
end

if 0
  p "0 truthy"      # Ruby: 0 is truthy
end

if []
  p "[] truthy"     # Ruby: [] is truthy
end

if false
  p "false truthy?"
else
  p "false falsy"
end

# to_s
p 42.to_s            # "42"
p (-7).to_s          # "-7"
p "abc".to_s         # "abc"  (no quotes — top-level to_s)
p [1, 2, 3].to_s     # "[1, 2, 3]"
p [1, "a", nil].to_s # "[1, \"a\", nil]"
p nil.to_s           # ""
p true.to_s          # "true"
p false.to_s         # "false"

# to_i
p "42".to_i          # 42
p "  -7xyz".to_i     # -7  (Ruby: leading whitespace + digits, stop at non-digit)
p "abc".to_i         # 0
p 42.to_i            # 42

# String order compare
p "abc" < "abd"      # true
p "abc" < "abc"      # false
p "abc" <= "abc"     # true
p "abc" > "abb"      # true
p "ab"  < "abc"      # true (prefix)

# String slice s[i, n]
p "hello"[1, 3]      # "ell"
p "hello"[0, 2]      # "he"
p "hello"[-3, 2]     # "ll"
p "hello"[10, 2]     # nil
p "hello"[2, 100]    # "llo" (clamped)
p "hello"[2, 0]      # ""
p "hello"[2, -1]     # nil

# Array slice a[i, n]
p [10, 20, 30, 40][1, 2]    # [20, 30]
p [10, 20, 30, 40][-2, 2]   # [30, 40]
p [10, 20, 30][10, 1]       # nil

# Interpolation
x = 42
p "x = #{x}"                # "x = 42"
p "#{1 + 2} and #{[3, 4]}"  # "3 and [3, 4]"
p ""                         # ""
p "no interp"                # "no interp"
p "#{nil} z"                 # " z" (nil.to_s = "")
