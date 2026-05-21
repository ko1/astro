# string operations

s = "hello"
p s
p s.size

# index
p s[0]
p s[1]
p s[4]

# concat
p s + " world"
p "a" + "b" + "c"

# repeat
p "ab" * 3
p "" * 5
p "x" * 0

# SSO boundary (= 7 chars)
p "1234567"          # SSO
p "12345678"         # heap
p "1234567" + "8"    # heap result

# concat long
a = "Hello, "
b = "world!"
c = a + b
p c
p c.size

# str_cmp
p "abc" == "abc"
p "abc" == "abd"
p "" == ""
p "abc" < "abd"
p "abd" > "abc"

# to_s / to_i
p 42.to_s
p "42".to_i
p "-7".to_i
