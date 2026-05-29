# AnCaml language spec (implemented subset of MinCaml)

`ancaml` implements MinCaml as defined by the reference implementation at
<https://esumii.github.io/min-caml/> (the `syntax.ml` / `parser.mly` /
`typing.ml` of the course compiler).  This document records exactly what
`ancaml` accepts and how it behaves.

## Grammar

A program is a single expression.  Precedence is given low → high; this
matches MinCaml's `parser.mly` precedence declarations.

```
exp   ::= let-form | if-form | exp ';' exp | put
let-form ::= 'let' ident '=' exp 'in' exp
          |  'let' 'rec' ident ident+ '=' exp 'in' exp        (* function *)
          |  'let' '(' ident (',' ident)+ ')' '=' exp 'in' exp (* tuple destructure *)
if-form  ::= 'if' exp 'then' exp 'else' exp
put   ::= tuple ('<-' exp)?         (* only when tuple is `simple.(idx)` *)
tuple ::= cmp (',' cmp)*            (* 2+ elements → a tuple *)
cmp   ::= add (('='|'<>'|'<'|'>'|'<='|'>=') add)*
add   ::= mul (('+'|'-'|'+.'|'-.') mul)*
mul   ::= unary (('*.'|'/.') unary)*
unary ::= '-' unary | '-.' unary | 'not' unary | app
app   ::= 'Array.create' dot dot | dot dot*
dot   ::= atom ('.(' exp ')')*
atom  ::= int | float | 'true' | 'false' | '(' ')' | '(' exp ')' | ident
```

`Array.make` is accepted as a synonym for `Array.create`.

## Lexical syntax

- **int**: `[0-9]+`.  Values within `int32` use a compact literal node;
  wider literals use a 64-bit node.  Runtime ints are 63-bit (tagged).
- **float**: a numeric literal containing a `.` and/or an exponent
  (`1.0`, `.5`-style is *not* accepted — a leading digit is required, as in
  MinCaml; `2.`, `3.14`, `1e10`, `2.5e-3` are).
- **ident**: `[A-Za-z_][A-Za-z0-9_']*`.
- **keywords**: `let rec in if then else true false not`.
- **comments**: `(* ... *)`, nestable.
- **operators**: `+ - +. -. *. /. = < > <= >= <> ( ) , ; . <-`.

## Types (`Type.t`)

```
unit | bool | int | float | t array | (t * t * ...) | (t -> t -> ... -> t)
```

No type annotations appear in the surface syntax; everything is inferred by
monomorphic Hindley–Milner unification (see [mincac_impl_spec.md](mincac_impl_spec.md)
§4).  A type variable left unresolved after inference defaults to `int`, as
in MinCaml.

## Evaluation semantics

- **Arithmetic.** `+ - (unary -)` operate on `int`.  `+. -. *. /. (unary -.)`
  operate on `float`.  There is **no integer `*` or `/`** (a deliberate
  MinCaml restriction — the lexer rejects bare `*` / `/`).
- **Comparison.** `=` `<>` `<` `>` `<=` `>=` are polymorphic and structural
  (like OCaml's), but the parser keeps only `Eq` / `LE` / `Not`:
  `a<>b ≡ not (a=b)`, `a<b ≡ not (b<=a)`, `a>b ≡ not (a<=b)`,
  `a>=b ≡ b<=a`.  Comparing a functional value is a runtime error.
- **`if`** requires a `bool` condition; both branches must unify.
- **`let` / `let rec`.** `let` is monomorphic.  `let rec f x… = e` defines a
  (possibly self-recursive) function; MinCaml has no mutual `and`, no
  anonymous `fun`, and a `let rec` must have ≥1 parameter.
- **Application** is multi-argument and exact-arity (no currying / partial
  application — that would be an arity type error).
- **Tuples** are immutable; `let (a, b, …) = e in …` destructures.
- **Arrays.** `Array.create n init` allocates `n` cells (all `init`),
  `a.(i)` reads, `a.(i) <- v` writes (returns `unit`).  Out-of-bounds access
  is a runtime error.
- **Sequencing.** `e1; e2` evaluates `e1` (which must have type `unit`) for
  effect, then `e2`.

## External functions

The only free names a program may reference are these built-ins (anything
else is an "unbound variable" parse error — `ancaml` does not treat unknown
names as externals the way the MinCaml compiler does, see
[todo.md](todo.md)):

| name | type |
|---|---|
| `print_int` | `int -> unit` |
| `print_char` | `int -> unit` (prints the byte) |
| `print_newline` | `unit -> unit` |
| `print_float` | `float -> unit` (OCaml `string_of_float` formatting) |
| `read_int` | `unit -> int` |
| `read_float` | `unit -> float` |
| `float_of_int` | `int -> float` |
| `int_of_float`, `truncate` | `float -> int` (toward zero) |
| `abs_float`, `sqrt`, `sin`, `cos`, `atan`, `floor` | `float -> float` |

## Differences from OCaml that matter for differential testing

Every valid `ancaml` program is valid OCaml with the same result **after** a
two-line prelude (see [testing.md](testing.md)):

1. `print_char` takes an `int` in MinCaml but a `char` in OCaml.
2. `Array.create` was removed from modern OCaml; `Array.make` replaces it.

`ancaml` rejects (statically) some programs OCaml accepts — e.g. integer
multiplication, mixed int/float arithmetic, non-`unit` left of `;` — because
MinCaml's type discipline is stricter.
