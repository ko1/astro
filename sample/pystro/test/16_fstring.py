# f-string format specs
print(f"pi={3.14159:.2f}")
print(f"e={2.71828:.4f}")
print(f"width={42:5d}")
print(f"zero-pad={42:05d}")
print(f"left=|{chr(65):<5}|")
print(f"right=|{chr(65):>5}|")
print(f"center=|{chr(65):^5}|")
print(f"hex={255:x}")
print(f"hex-up={255:X}")
print(f"bin={10:b}")
print(f"oct={64:o}")
print(f"scientific={123456.789:.2e}")
print(f"general={0.000123:g}")

# expression in fstring
x = 7
print(f"x={x}, x*x={x*x}")
print(f"sum={sum(range(10))}")

# nested braces (escape)
print(f"set={{1, 2, 3}}")
