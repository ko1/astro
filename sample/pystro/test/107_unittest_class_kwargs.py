import unittest


class ClassKwargsTest(unittest.TestCase):
    def test_init_subclass_receives_kwargs(self):
        log = {}
        class Plugin:
            def __init_subclass__(cls, name=None, **kw):
                log[name] = (cls.__name__, dict(kw))

        class JSON(Plugin, name="json", priority=1): pass
        class XML(Plugin, name="xml"): pass

        self.assertEqual(log["json"][0], "JSON")
        self.assertEqual(log["json"][1], {"priority": 1})
        self.assertEqual(log["xml"][0], "XML")
        self.assertEqual(log["xml"][1], {})

    def test_register_pattern(self):
        registry = {}
        class Base:
            def __init_subclass__(cls, *, name, **kw):
                registry[name] = cls
        class A(Base, name="a"): pass
        class B(Base, name="b"): pass
        self.assertIs(registry["a"], A)
        self.assertIs(registry["b"], B)


unittest.main(globals())
