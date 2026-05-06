import unittest


class LambdaClosureTest(unittest.TestCase):
    def test_simple_capture(self):
        g = lambda f: lambda n: f
        res = g("hello")
        self.assertEqual(res(0), "hello")
        self.assertEqual(res(99), "hello")
        self.assertEqual(res("world"), "hello")

    def test_y_combinator_factorial(self):
        # Classic Y combinator pattern — exercises closure capture
        # across two nested lambdas with self-application.
        fact = (lambda f: lambda n: 1 if n <= 1 else n * f(f)(n - 1))(
                lambda f: lambda n: 1 if n <= 1 else n * f(f)(n - 1))
        self.assertEqual(fact(5), 120)
        self.assertEqual(fact(10), 3628800)
        self.assertEqual(fact(0), 1)

    def test_returned_lambda_independent_calls(self):
        # Each call returns a lambda; the lambda captures the call's
        # outer parameter, not a stale one.
        make = lambda x: lambda: x
        a = make(1)
        b = make(2)
        c = make(3)
        self.assertEqual(a(), 1)
        self.assertEqual(b(), 2)
        self.assertEqual(c(), 3)

    def test_lambda_in_list(self):
        # Closure captured in a list survives parent lambda return.
        fns = [(lambda i=i: lambda: i)() for i in range(3)]
        self.assertEqual([f() for f in fns], [0, 1, 2])

    def test_three_level_nest(self):
        f = lambda a: lambda b: lambda c: a + b + c
        self.assertEqual(f(1)(2)(3), 6)


unittest.main(globals())
