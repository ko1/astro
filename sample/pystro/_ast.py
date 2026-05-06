# pystro stub for `_ast` (CPython C extension).  We don't model AST
# nodes, so this exposes minimal placeholder classes that satisfy
# `from _ast import *` and the type-checks done by ast.py.

class AST: pass

# Common AST node classes — pystro doesn't construct these; they only
# need to exist as types for isinstance checks.
class mod(AST): pass
class Module(mod): pass
class Interactive(mod): pass
class Expression(mod): pass

class stmt(AST): pass
class FunctionDef(stmt): pass
class AsyncFunctionDef(stmt): pass
class ClassDef(stmt): pass
class Return(stmt): pass
class Delete(stmt): pass
class Assign(stmt): pass
class TypeAlias(stmt): pass
class AugAssign(stmt): pass
class AnnAssign(stmt): pass
class For(stmt): pass
class AsyncFor(stmt): pass
class While(stmt): pass
class If(stmt): pass
class With(stmt): pass
class AsyncWith(stmt): pass
class Match(stmt): pass
class Raise(stmt): pass
class Try(stmt): pass
class TryStar(stmt): pass
class Assert(stmt): pass
class Import(stmt): pass
class ImportFrom(stmt): pass
class Global(stmt): pass
class Nonlocal(stmt): pass
class Expr(stmt): pass
class Pass(stmt): pass
class Break(stmt): pass
class Continue(stmt): pass

class expr(AST): pass
class BoolOp(expr): pass
class NamedExpr(expr): pass
class BinOp(expr): pass
class UnaryOp(expr): pass
class Lambda(expr): pass
class IfExp(expr): pass
class Dict(expr): pass
class Set(expr): pass
class ListComp(expr): pass
class SetComp(expr): pass
class DictComp(expr): pass
class GeneratorExp(expr): pass
class Await(expr): pass
class Yield(expr): pass
class YieldFrom(expr): pass
class Compare(expr): pass
class Call(expr): pass
class FormattedValue(expr): pass
class JoinedStr(expr): pass
class Constant(expr): pass
class Attribute(expr): pass
class Subscript(expr): pass
class Starred(expr): pass
class Name(expr): pass
class List(expr): pass
class Tuple(expr): pass
class Slice(expr): pass

class TemplateStr(expr): pass
class Interpolation(expr): pass

class expr_context(AST): pass
class Load(expr_context): pass
class Store(expr_context): pass
class Del(expr_context): pass

class boolop(AST): pass
class And(boolop): pass
class Or(boolop): pass

class operator(AST): pass
class Add(operator): pass
class Sub(operator): pass
class Mult(operator): pass
class MatMult(operator): pass
class Div(operator): pass
class Mod(operator): pass
class Pow(operator): pass
class LShift(operator): pass
class RShift(operator): pass
class BitOr(operator): pass
class BitXor(operator): pass
class BitAnd(operator): pass
class FloorDiv(operator): pass

class unaryop(AST): pass
class Invert(unaryop): pass
class Not(unaryop): pass
class UAdd(unaryop): pass
class USub(unaryop): pass

class cmpop(AST): pass
class Eq(cmpop): pass
class NotEq(cmpop): pass
class Lt(cmpop): pass
class LtE(cmpop): pass
class Gt(cmpop): pass
class GtE(cmpop): pass
class Is(cmpop): pass
class IsNot(cmpop): pass
class In(cmpop): pass
class NotIn(cmpop): pass

class comprehension(AST): pass
class ExceptHandler(AST): pass
class arguments(AST): pass
class arg(AST): pass
class keyword(AST): pass
class alias(AST): pass
class withitem(AST): pass
class match_case(AST): pass

class pattern(AST): pass
class MatchValue(pattern): pass
class MatchSingleton(pattern): pass
class MatchSequence(pattern): pass
class MatchMapping(pattern): pass
class MatchClass(pattern): pass
class MatchStar(pattern): pass
class MatchAs(pattern): pass
class MatchOr(pattern): pass

class type_param(AST): pass
class TypeVar(type_param): pass
class ParamSpec(type_param): pass
class TypeVarTuple(type_param): pass


# Module-level constants.
PyCF_ONLY_AST = 0x0400
PyCF_TYPE_COMMENTS = 0x1000
PyCF_ALLOW_TOP_LEVEL_AWAIT = 0x2000
PyCF_OPTIMIZED_AST = 0x8000
