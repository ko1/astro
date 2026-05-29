# MinCaml — a self-contained reimplementation reference

This document specifies the MinCaml dialect that `ancaml` implements precisely
enough to **reimplement from scratch by any method** (another tree walker, a
bytecode VM, a compiler) and check against the same differential suite
([testing.md](testing.md)).  It follows the reference implementation at
<https://esumii.github.io/min-caml/> (`syntax.ml`, `parser.mly`, `typing.ml`),
noting where `ancaml` makes a deliberate, documented choice.

MinCaml is intentionally tiny: **monomorphic**, no strings, no lists, no
variants, no records, no `and`, no anonymous functions.  A whole program is
one expression.

---

## 1. Lexical structure

Tokens, longest-match:

- `int` — `[0-9]+`.
- `float` — a numeric literal that contains a `.` or an exponent:
  `[0-9]+ ('.' [0-9]*)? ([eE] [+-]? [0-9]+)?` where the `.`/exponent is what
  distinguishes it from an `int` (so `123` is an int, `123.`, `1.5`, `1e3`
  are floats).  A leading digit is required.
- `ident` — `[A-Za-z_][A-Za-z0-9_']*`, minus the keywords.
- keywords — `let rec in if then else true false not`.
- the two-token-glued literal `Array.create` and its synonym `Array.make`.
- operators / punctuation — `+ - +. -. *. /. = < > <= >= <> ( ) , ; . <-`.
- comments — `(* … *)`, **nested**.  Whitespace is insignificant.

Note there is **no** bare `*` or `/` token: integer multiplication/division
does not exist in MinCaml.  An implementation should reject them.

---

## 2. Concrete syntax & precedence

A program is one `exp`.  Precedence low → high (from MinCaml's `parser.mly`):

```
IN  <  let  <  ';'  <  if  <  '<-'  <  tuple(',')  <  (= <> < > <= >=)
    <  (+ - +. -.)  <  (*. /.)  <  unary(- -. not)  <  application  <  '.('
```

Productions:

```
exp     ::= 'let' ident '=' exp 'in' exp
          | 'let' 'rec' ident ident+ '=' exp 'in' exp
          | 'let' '(' ident (',' ident)+ ')' '=' exp 'in' exp
          | 'if' exp 'then' exp 'else' exp
          | exp ';' exp
          | simple '.' '(' exp ')' '<-' exp
          | exp ',' exp (',' exp)*                  (* tuple, ≥2 *)
          | exp ('='|'<>'|'<'|'>'|'<='|'>=') exp
          | exp ('+'|'-'|'+.'|'-.') exp
          | exp ('*.'|'/.') exp
          | '-' exp | '-.' exp | 'not' exp
          | exp simple+                              (* application *)
          | 'Array.create' simple simple
          | simple
simple  ::= int | float | 'true' | 'false' | '(' ')' | '(' exp ')'
          | ident | simple '.' '(' exp ')'
```

Parsing notes that bite (all reproduced by `ancaml`'s `parse.c`):

- `;` is the second-lowest precedence (only `let` is lower), so `let x=e in a; b`
  parses as `let x=e in (a; b)`, while `(if c then x else y); z` requires the
  `;` to be *outside* the `if`.
- `<-` binds tighter than `;` but looser than tuple: `a.(i) <- x, y; z`
  parses as `(a.(i) <- (x, y)); z`.
- Unary `-` applied to a **float literal** folds to a negative float literal
  (`-3.14` is a float); applied to anything else it is integer negation
  (`- x` is `Neg x`, an int op).  Use `-.` for float negation of non-literals.
- Comparisons desugar (see §3) so only `Eq`, `LE`, `Not` reach the evaluator.

---

## 3. Abstract syntax (after desugaring)

```
e ::= Unit | Bool b | Int n | Float f
    | Var x
    | Not e | Neg e | Add e e | Sub e e
    | FNeg e | FAdd e e | FSub e e | FMul e e | FDiv e e
    | Eq e e | LE e e
    | If e e e
    | Let x e e
    | LetRec { name=f; args=[x…]; body=e } e
    | App e [e…]
    | Tuple [e…]
    | LetTuple [x…] e e
    | Array e e            (* size, init *)
    | Get e e              (* arr, idx *)
    | Put e e e            (* arr, idx, value *)
```

Comparison desugaring performed by the parser:

| surface | AST |
|---|---|
| `a = b`  | `Eq a b` |
| `a <> b` | `Not (Eq a b)` |
| `a <= b` | `LE a b` |
| `a >= b` | `LE b a` |
| `a < b`  | `Not (LE b a)` |
| `a > b`  | `Not (LE a b)` |
| `e1 ; e2` | `Let _ e1 e2` with `_ : unit` (ancaml uses a dedicated `Seq`) |

`ancaml` additionally resolves `Var x` into either a de Bruijn `(depth, idx)`
local reference or an external reference at parse time; a from-scratch
reimplementation may keep names and resolve at eval time instead — the
observable behaviour is identical.

---

## 4. Type system — monomorphic Hindley–Milner

Types: `unit | bool | int | float | fun([t…], t) | tuple([t…]) | array(t) | var(ref)`.

A `var` is a mutable cell holding either nothing (unbound) or another type.
Inference is **destructive unification**, **no generalization** (let is
monomorphic).  `infer(Γ, e)` returns a type and unifies as a side effect:

```
infer(Γ, Unit) = unit
infer(Γ, Bool) = bool        infer(Γ, Int) = int        infer(Γ, Float) = float
infer(Γ, Var x) = Γ(x)       (* external x: its fixed signature, §6 *)
infer(Γ, Not e)  = unify(infer e, bool);  bool
infer(Γ, Neg e)  = unify(infer e, int);   int
infer(Γ, Add/Sub a b)         = unify(infer a, int);  unify(infer b, int);  int
infer(Γ, FNeg e)              = unify(infer e, float); float
infer(Γ, FAdd/FSub/FMul/FDiv) = unify both float;      float
infer(Γ, Eq a b) = unify(infer a, infer b); bool
infer(Γ, LE a b) = unify(infer a, infer b); bool
infer(Γ, If c t e) = unify(infer c, bool); let tt=infer t, te=infer e in unify(tt,te); tt
infer(Γ, Let x e1 e2)  = let t1=infer e1 in infer(Γ{x:t1}, e2)
infer(Γ, LetRec{f;args;body} e2):
        let tf = newvar(); each arg gets newvar() ti;
        let tr = infer(Γ{f:tf, args:ti}, body);
        unify(tf, fun([ti], tr));
        infer(Γ{f:tf}, e2)
infer(Γ, App f [a…]):  let tf=infer f; let ta=map infer a; let tr=newvar();
                       unify(tf, fun(ta, tr)); tr
infer(Γ, Tuple [e…]) = tuple(map infer e)
infer(Γ, LetTuple [x…] e1 e2):
        let ti = newvar() for each x; unify(infer e1, tuple(ti));
        infer(Γ{x:ti}, e2)
infer(Γ, Array n init) = unify(infer n, int); array(infer init)
infer(Γ, Get a i)      = unify(infer i, int); let ev=newvar(); unify(infer a, array ev); ev
infer(Γ, Put a i v)    = unify(infer i, int); let tv=infer v; unify(infer a, array tv); unit
```

`unify(a, b)` after dereferencing top-level vars:

- equal → done; one side is an unbound var → occurs-check the other, then
  point the var at it; both `fun` → arities equal then unify args pairwise and
  the results; both `tuple` → widths equal then unify elementwise; both
  `array` → unify elements; same base kind → done; otherwise **type error**.

After inference, any var still unbound conceptually defaults to `int`
(MinCaml).  An interpreter that doesn't consult types at runtime (like
`ancaml`) only needs this for display.

`ancaml` reports the **first** unification failure with the source line and
aborts (matching MinCaml's "stop at first type error"); the program is not
run unless it type-checks.

---

## 5. Dynamic semantics

Call-by-value, left-to-right.  Values: `()`, `true`/`false`, an integer, a
float, a closure, a tuple, an array.

- `Add/Sub/Neg`: integer arithmetic.  `FAdd/FSub/FMul/FDiv/FNeg`: float.
- `Eq`: **structural** equality (ints/floats/bools/units by value; tuples and
  arrays elementwise; comparing a closure is a runtime error).  `LE`: the
  corresponding total order (ints/floats numerically; tuples/arrays
  lexicographically; `false < true`).  These match OCaml's polymorphic `=`/`<=`.
- `If`: evaluate the condition; run the matching branch.
- `Let`: bind, then body.  `LetRec`: bind `f` to a closure that captures an
  environment in which `f` is itself visible (enabling recursion), then body.
- `App`: evaluate callee to a closure, evaluate all arguments, bind the
  parameters, evaluate the body.  Arity always matches (guaranteed by typing).
- `Tuple`: evaluate elements left-to-right.  `LetTuple`: evaluate the tuple,
  bind its components.
- `Array n init`: allocate `n` cells each `init`.  `Get`/`Put`: bounds-checked
  (out-of-bounds is a runtime error).  `Put` returns `()`.
- `Seq`/`;`: evaluate the first (a `unit`) for effect, return the second.

---

## 6. External functions

The fixed external signatures (the only free names allowed):

```
print_int     : int   -> unit       print_char    : int   -> unit   (* prints a byte *)
print_newline : unit  -> unit        print_float   : float -> unit
read_int      : unit  -> int         read_float    : unit  -> float
float_of_int  : int   -> float       int_of_float  : float -> int    (* toward zero *)
truncate      : float -> int         abs_float     : float -> float
sqrt sin cos atan floor : float -> float
```

`print_int` prints with no trailing newline; `print_float` matches OCaml's
`string_of_float` (`%.12g`, with a trailing `.` appended when the rendering
has no `.`/exponent).  These let MinCaml output match OCaml's verbatim.

---

## 7. The OCaml-differential contract

Because MinCaml is a near-subset of OCaml, the reference oracle is the system
`ocaml`, with a two-line prelude reconciling the only surface differences
(`print_char`'s argument type and the removed `Array.create`):

```ocaml
let print_char n = print_char (Char.chr n)
module Array = struct include Stdlib.Array let create = make end
let _ = ( <program> )
```

A conforming implementation must, for every well-typed program, produce
stdout byte-identical to the above.  See [testing.md](testing.md) for the
harness (`ANCAML=... ruby test/run_tests.rb`) and the portability rules for
test programs.
