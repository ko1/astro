# AnLox language spec (implemented Lox)

AnLox implements **Lox** as defined by Robert Nystrom's *Crafting Interpreters*
(<https://craftinginterpreters.com/>), at the level of the book's tree-walking
interpreter ("jlox", parts I–II through inheritance).  This documents what
AnLox accepts and how it behaves.

## Lexical syntax

- **numbers**: `[0-9]+ ("." [0-9]+)?` — no leading/trailing dot, no exponent.
  All numbers are IEEE doubles.
- **strings**: `"..."` — no escape sequences; may span lines.
- **identifiers**: `[A-Za-z_][A-Za-z0-9_]*`.
- **keywords**: `and class else false for fun if nil or print return super this
  true var while`.
- **comments**: `//` to end of line. (No block comments.)
- **operators / punctuation**: `( ) { } , . ; + - * / ! != = == > >= < <=`.

## Grammar (statements & expressions)

```
program     → declaration* EOF
declaration → varDecl | funDecl | classDecl | statement
varDecl     → "var" IDENT ( "=" expression )? ";"
funDecl     → "fun" function
classDecl   → "class" IDENT ( "<" IDENT )? "{" function* "}"
function    → IDENT "(" parameters? ")" block
statement   → exprStmt | printStmt | block | ifStmt | whileStmt | forStmt | returnStmt
block       → "{" declaration* "}"
ifStmt      → "if" "(" expression ")" statement ( "else" statement )?
whileStmt   → "while" "(" expression ")" statement
forStmt     → "for" "(" ( varDecl | exprStmt | ";" ) expression? ";" expression? ")" statement
returnStmt  → "return" expression? ";"

expression  → assignment
assignment  → ( call "." )? IDENT "=" assignment | logic_or
logic_or    → logic_and ( "or" logic_and )*
logic_and   → equality ( "and" equality )*
equality    → comparison ( ("=="|"!=") comparison )*
comparison  → term ( ("<"|"<="|">"|">=") term )*
term        → factor ( ("+"|"-") factor )*
factor      → unary ( ("*"|"/") unary )*
unary       → ("!"|"-") unary | call
call        → primary ( "(" arguments? ")" | "." IDENT )*
primary     → NUMBER | STRING | "true" | "false" | "nil" | "this"
            | IDENT | "(" expression ")" | "super" "." IDENT
```

## Semantics

- **Types**: `nil`, booleans, numbers (double), strings, functions/closures,
  classes, instances.
- **Truthiness**: only `nil` and `false` are falsey; everything else (incl. `0`
  and `""`) is truthy.
- **`+`** is overloaded: number addition or string concatenation; mixing is a
  runtime error.  `- * /` and comparisons require numbers.
- **`==` / `!=`** never error: numbers/strings compare by value, other objects
  by identity; different types are unequal.
- **`and` / `or`** short-circuit and evaluate to an operand (not a coerced bool).
- **Variables**: `var` at top level declares a (late-bound, redefinable) global;
  inside a block/function it declares a block-scoped local.  Reading an
  undefined global is a runtime error; reading a local in its own initializer
  is a compile error.
- **Functions** are first-class closures capturing their defining scope.
  Wrong-arity calls are a runtime error.  `return` exits a function (bare
  `return;` yields `nil`).
- **Classes**: methods, dynamic instance fields (`obj.field = …` creates them),
  `this`, an optional `init` constructor (calling the class constructs and runs
  it, returning the instance), single inheritance (`class B < A`), and `super`
  for superclass-method dispatch.  Accessing a method yields a bound method
  (its `this` is fixed).

## Exit codes (matching the book)

`65` for a compile/parse error, `70` for a runtime error, `0` on success.

## Differences / simplifications from the book

- Number printing uses `printf`-style formatting (integers without a decimal
  point; others via `%.10g`).  jlox uses Java's `Double.toString`, so many-digit
  decimals may differ.
- The resolver enforces the common static errors (use-in-own-initializer,
  duplicate local, `return`/`this`/`super` misuse, self-inheritance) but is not
  an exhaustive port of every diagnostic.
- A value-returning `return` inside `init` is accepted (the book rejects it).
- No standalone `--build` executable (the AST uses side-table indirection the
  framework's embedder doesn't reconstruct); the interpreter and `--aot-compile`
  modes are fully supported.  See [todo.md](todo.md).
