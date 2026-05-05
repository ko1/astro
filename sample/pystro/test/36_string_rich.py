# Rich string operations / methods / slicing.

s = "Hello, World!"
print(len(s))
print(s.upper())
print(s.lower())
print(s.startswith("Hello"))
print(s.endswith("!"))
print(s.find("World"))
print(s.find("xyz"))
print(s.count("l"))
print(s.replace("World", "Universe"))

# Indexing.
print(s[0])
print(s[-1])
print(s[7])

# Slicing.
print(s[:5])
print(s[7:12])
print(s[-6:-1])
print(s[::2])
print(s[::-1])
print(s[1:9:2])

# Iteration.
out = []
for ch in "abc":
    out.append(ch)
print(out)

# split / join.
csv = "a,b,c,d,e"
parts = csv.split(",")
print(parts)
print(",".join(parts))
print("-".join(["x", "y", "z"]))

# strip.
print("  hello  ".strip())
print("\t\nspaced\n\t".strip())
print("---hello---".strip("-"))

# Concatenation.
print("ab" + "cd")
print("x" * 5)
print(5 * "y")
print("" * 100)
print("ab" * 0)

# Comparison.
print("abc" == "abc")
print("abc" == "abd")
print("abc" < "abd")
print("abc" <= "abc")
print("Z" > "A")

# Membership.
print("ell" in s)
print("xyz" in s)
print("H" in s)

# str() conversions.
print(str(123))
print(str(3.14))
print(str(True))
print(str(None))
print(str([1, 2, 3]))

# repr.
print(repr("with \"quotes\""))
print(repr("with 'quotes'"))
print(repr(None))
print(repr([1, "two", 3.0]))

# Empty string.
print(len(""))
print("" == "")
print("".upper())

# ord / chr.
print(ord("A"))
print(chr(65))
print(chr(ord("a") + 25))

# Escapes in literals.
print("a\tb")
print("\n".join(["line1", "line2"]))
print("backslash: \\")
print("'single'")
print('"double"')

# Format with width.
print("{:>10}".format("hi"))
print("{:<10}|".format("hi"))
print("{:^10}|".format("hi"))
print("{:0>5}".format(42))

