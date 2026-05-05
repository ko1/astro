# Module init raise propagates to caller's try/except.

print("before")
try:
    import badmod
    print("imported OK (should not see this)")
except ValueError as e:
    print("caught ValueError")
print("after")

# Re-importing a previously-failed module re-runs init (Python actually
# caches by name on first failure too, but for pystro we just re-run —
# the important thing is it raises again, not silently succeeds).
try:
    import badmod
    print("retry imported OK (should not see this)")
except ValueError:
    print("retry caught")

# Catching as base Exception still works — same class identity across modules.
try:
    import badmod
except Exception as e:
    print("caught as Exception")

# After all that, normal import still works.
import mathmod
print(mathmod.PI)
print(mathmod.square(3))
