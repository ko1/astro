class Resource:
    def __init__(self, name):
        self.name = name
    def __enter__(self):
        print("entering", self.name)
        return self.name
    def __exit__(self, t, v, tb):
        print("exiting", self.name)
        return False

with Resource("foo") as r:
    print("body", r)

# without `as`
with Resource("bar"):
    print("body bar")

# nested
with Resource("outer") as o:
    with Resource("inner") as i:
        print("inside", o, i)

# exception inside with — finally runs
try:
    with Resource("crash") as r:
        raise ValueError("boom")
except ValueError as e:
    print("caught:", e.message)

# with on a function-defined CM
def make_cm(tag):
    class CM:
        def __enter__(self):
            print("enter", tag)
            return tag
        def __exit__(self, *args):
            print("exit", tag)
            return False
    return CM()

with make_cm("T") as t:
    print("got", t)
