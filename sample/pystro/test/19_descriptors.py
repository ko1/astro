class Counter:
    count = 0    # NOTE: pystro doesn't bind class-level attrs (other than methods)

    @classmethod
    def increment(cls):
        # Without class-attr storage we just return a probe; this
        # exercises the classmethod binding.
        return cls

    @staticmethod
    def double(x):
        return x * 2

    def __init__(self, name):
        self._name = name

    @property
    def name(self):
        return self._name + "!"

    @property
    def upper_name(self):
        return self.name.upper()

# staticmethod via class
print(Counter.double(7))

# classmethod via class — receives the class
print(Counter.increment())   # <class 'Counter'>

# staticmethod via instance
c = Counter("alice")
print(c.double(8))

# classmethod via instance
print(c.increment())

# property via instance
print(c.name)
print(c.upper_name)

# subclass classmethod gets subclass as `cls`
class Sub(Counter):
    pass

print(Sub.increment())   # <class 'Sub'>

# property reads through inheritance
s = Sub("bob")
print(s.name)
