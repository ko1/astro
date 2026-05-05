import unittest


class ExpressionEvaluatorTest(unittest.TestCase):
    def test_recursive_descent(self):
        class Token:
            def __init__(self, kind, value):
                self.kind = kind
                self.value = value

        def tokenize(s):
            tokens = []
            i = 0
            while i < len(s):
                ch = s[i]
                if ch in " \t":
                    i += 1; continue
                if ch.isdigit() or ch == ".":
                    j = i
                    while j < len(s) and (s[j].isdigit() or s[j] == "."):
                        j += 1
                    tokens.append(Token("NUM", float(s[i:j])))
                    i = j; continue
                if ch in "+-*/()":
                    tokens.append(Token(ch, ch))
                    i += 1; continue
                raise ValueError("bad char")
            return tokens

        class Parser:
            def __init__(self, tokens):
                self.tokens = tokens; self.pos = 0
            def peek(self):
                return self.tokens[self.pos] if self.pos < len(self.tokens) else None
            def eat(self):
                t = self.tokens[self.pos]; self.pos += 1; return t
            def expr(self):
                left = self.term()
                while self.peek() and self.peek().kind in "+-":
                    op = self.eat().kind
                    right = self.term()
                    left = (left + right) if op == "+" else (left - right)
                return left
            def term(self):
                left = self.factor()
                while self.peek() and self.peek().kind in "*/":
                    op = self.eat().kind
                    right = self.factor()
                    left = (left * right) if op == "*" else (left / right)
                return left
            def factor(self):
                t = self.peek()
                if t.kind == "(":
                    self.eat(); v = self.expr(); self.eat(); return v
                return self.eat().value

        def evaluate(s):
            return Parser(tokenize(s)).expr()

        self.assertAlmostEqual(evaluate("1 + 2"), 3.0)
        self.assertAlmostEqual(evaluate("2 * 3 + 4"), 10.0)
        self.assertAlmostEqual(evaluate("(1+2) * 3"), 9.0)
        self.assertAlmostEqual(evaluate("10 / 4"), 2.5)
        self.assertAlmostEqual(evaluate("((1+2)*(3+4))-5"), 16.0)


class TransactionPatternTest(unittest.TestCase):
    def test_with_rollback(self):
        class Tx:
            def __init__(self, store):
                self.store = store
                self.changes = []
            def __enter__(self):
                return self
            def __exit__(self, et, ev, tb):
                if et is not None:
                    for k, v in reversed(self.changes):
                        if v is None:
                            if k in self.store: del self.store[k]
                        else:
                            self.store[k] = v
                self.changes = []
                return False
            def set(self, k, v):
                old = self.store.get(k, None)
                self.store[k] = v
                self.changes.append((k, old))

        d = {"a": 1, "b": 2}
        try:
            with Tx(d) as tx:
                tx.set("a", 99)
                tx.set("c", 3)
                raise RuntimeError("oops")
        except RuntimeError:
            pass
        self.assertEqual(d, {"a": 1, "b": 2})


class DataclassWithKwargsTest(unittest.TestCase):
    def test_dataclass_kwargs(self):
        from dataclasses import dataclass
        @dataclass
        class Point:
            x: int
            y: int

        p = Point(**{"x": 1, "y": 2})
        self.assertEqual((p.x, p.y), (1, 2))


class GeneratorPipelineTest(unittest.TestCase):
    def test_dedupe_via_gen(self):
        def dedupe(items):
            seen = set()
            for x in items:
                if x in seen: continue
                seen.add(x)
                yield x
        self.assertEqual(list(dedupe([1, 2, 1, 3, 2, 4])), [1, 2, 3, 4])


class ClassHierarchyTest(unittest.TestCase):
    def test_super_chain(self):
        class A:
            def m(self): return "A"
        class B(A):
            def m(self): return "B->" + super().m()
        class C(B):
            def m(self): return "C->" + super().m()
        self.assertEqual(C().m(), "C->B->A")


unittest.main(globals())
