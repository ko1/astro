# Standard library: math, sys, json.

import math
import sys
import json

# math.
print(round(math.pi * 100) / 100)
print(round(math.e * 100) / 100)
print(math.sqrt(144))
print(math.sqrt(2) > 1.41 and math.sqrt(2) < 1.42)
print(math.cos(0))
print(round(math.sin(math.pi / 2)))
print(math.floor(3.7))
print(math.ceil(3.2))
print(math.factorial(6))
print(math.gcd(36, 24))
print(math.fabs(-5))
print(math.fabs(5))
print(math.hypot(3, 4))
print(round(math.log(math.e) * 1000) / 1000)
print(round(math.log(100, 10)))
print(round(math.exp(1) * 1000) / 1000)
print(math.isclose(0.1 + 0.2, 0.3))

# sys (just make sure these don't throw).
print(isinstance(sys.argv, list))

# json round-trip.
data = {"name": "alice", "age": 30, "items": [1, 2.5, True, None, "x"]}
serialized = json.dumps(data)
print(serialized)
parsed = json.loads(serialized)
print(parsed["name"])
print(parsed["age"])
print(parsed["items"])
print(parsed["items"][2] is True)
print(parsed["items"][3] is None)

# Empty containers.
print(json.dumps([]))
print(json.dumps({}))
print(json.loads("[]"))
print(json.loads("{}"))

# Whitespace.
print(json.loads("  [ 1 , 2 ,\n3 ]  "))

# Nested.
nested = {"a": [{"b": 1}, {"b": 2}]}
print(json.loads(json.dumps(nested)))

# Strings with escapes.
s = json.loads('"line1\\nline2"')
print(repr(s))
print(json.dumps("with \"quotes\" and \\backslash"))

# Numbers.
print(json.loads("3.14"))
print(json.loads("-42"))
print(json.loads("1e3"))

# Booleans / null.
print(json.loads("true"))
print(json.loads("false"))
print(json.loads("null"))

# Errors.
try:
    json.loads("{")
except ValueError:
    print("err1")
try:
    json.loads("nope")
except ValueError:
    print("err2")
