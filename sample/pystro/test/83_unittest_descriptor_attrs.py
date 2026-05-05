import unittest


class PropertyDeleterTest(unittest.TestCase):
    def test_deleter(self):
        class T:
            def __init__(self): self._x = 1
            @property
            def x(self): return self._x
            @x.setter
            def x(self, v): self._x = v
            @x.deleter
            def x(self): self._x = None
        t = T()
        self.assertEqual(t.x, 1)
        t.x = 5
        self.assertEqual(t.x, 5)
        del t.x
        self.assertIsNone(t.x)


class CachedPropertyPatternTest(unittest.TestCase):
    def test_cached(self):
        calls = []
        class CP:
            def __init__(self, fn):
                self.fn = fn
                self.name = fn.__name__
            def __get__(self, obj, owner=None):
                if obj is None: return self
                v = self.fn(obj)
                obj.__dict__[self.name] = v
                return v
        class C:
            @CP
            def cost(self):
                calls.append(1)
                return 42
        c = C()
        self.assertEqual(c.cost, 42)
        self.assertEqual(c.cost, 42)
        self.assertEqual(c.cost, 42)
        self.assertEqual(len(calls), 1)


class DunderHooksTest(unittest.TestCase):
    def test_getattribute(self):
        log = []
        class S:
            def __init__(self): self.x = 1
            def __getattribute__(self, name):
                log.append(name)
                return object.__getattribute__(self, name)
        s = S()
        _ = s.x
        self.assertIn("x", log)

    def test_setattr(self):
        log = []
        class M:
            def __setattr__(self, name, val):
                log.append((name, val))
                object.__setattr__(self, name, val)
        m = M()
        m.foo = "bar"
        self.assertEqual(log, [("foo", "bar")])
        self.assertEqual(m.foo, "bar")

    def test_delattr(self):
        log = []
        class D:
            def __init__(self): self.x = 1
            def __delattr__(self, name):
                log.append(name)
                object.__delattr__(self, name)
        d = D()
        del d.x
        self.assertEqual(log, ["x"])
        self.assertFalse(hasattr(d, "x"))

    def test_getattr_fallback(self):
        class FB:
            def __init__(self): self.real = 1
            def __getattr__(self, name): return f"missing-{name}"
        fb = FB()
        self.assertEqual(fb.real, 1)
        self.assertEqual(fb.foo, "missing-foo")

    def test_dict_aliased(self):
        # obj.__dict__[k] = v should persist on the instance.
        class O:
            pass
        o = O()
        o.__dict__["xyz"] = 99
        self.assertEqual(o.xyz, 99)


unittest.main(globals())
