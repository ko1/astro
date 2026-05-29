# Lox — a reimplementation reference (AnLox's subset)

This is enough to **reimplement Lox by any method** and check it against the
same suite ([testing.md](testing.md)).  The authoritative source is Robert
Nystrom's *Crafting Interpreters* (<https://craftinginterpreters.com/>); this
records the exact subset AnLox targets and where it makes a documented choice.

## 1. Lexing

Longest-match tokens; `//`-to-EOL comments; whitespace insignificant.

- number `[0-9]+("."[0-9]+)?` (all numbers are IEEE doubles; no exponent form),
- string `"..."` (no escapes; may contain newlines),
- identifier `[A-Za-z_][A-Za-z0-9_]*` minus keywords,
- keywords: `and class else false for fun if nil or print return super this
  true var while`,
- `( ) { } , . ; + - * / ! != = == > >= < <=`.

## 2. Grammar

See [spec.md](spec.md) §Grammar for the full BNF.  Notes that bite:

- Assignment is right-associative and its target must be a variable or a
  `obj.field` get (else "Invalid assignment target.").
- `or` < `and` < equality < comparison < `+ -` < `* /` < unary < call/`.` .
- `for` desugars to `{ init?; while (cond) { body; incr? } }` in a fresh scope.
- A method is just `IDENT "(" params? ")" block` inside a class body; a method
  named `init` is the constructor.

## 3. Values & semantics

- Types: nil, bool, number (double), string, function/closure, class, instance,
  native.  **Truthy** = everything except `nil` and `false`.
- `+`: number add OR string concat (mixing → runtime error "Operands must be
  two numbers or two strings."); `- * /`, comparisons → numbers ("Operands must
  be numbers."); unary `-` → number ("Operand must be a number.").
- `== / !=`: never error; numbers & strings by value, other objects by
  identity, cross-type ⇒ not equal.
- `and`/`or`: short-circuit, evaluate to an operand.
- **Scope**: top-level `var` ⇒ a late-bound global (redefinable; undefined read
  ⇒ "Undefined variable 'x'."); inner `var` ⇒ block-local.  A resolver fixes
  each local reference to a (scope distance, slot); globals stay by-name.
  Reading a local inside its own initializer ⇒ compile error.
- **Functions**: first-class closures over their defining environment;
  wrong arity ⇒ "Expected N arguments but got M."; `return` exits (bare ⇒ nil).
- **Classes**: calling a class constructs an instance and runs `init` (if any),
  returning the instance.  Fields are dynamic (`o.f = v` creates them).
  Method access yields a **bound method** (its `this` is the receiver).
  `class B < A` inherits; method lookup walks the superclass chain; `super.m`
  starts the lookup in the superclass with the current `this`.  Superclass must
  be a class; a class can't inherit from itself.
- **Print**: `nil`/`true`/`false` as words; strings raw; numbers as integers
  when integer-valued (`3`, not `3.0`).
- **Exit codes**: 65 compile error, 70 runtime error, 0 ok.

## 4. A reference recipe (how AnLox does it)

Not prescriptive — any method that matches the observable behaviour passes.
AnLox is a tree walker:

1. **Lex** to a token stream.
2. **Parse + resolve in one pass**: recursive descent + precedence climbing,
   maintaining a scope stack; emit `(depth, slot)` for locals, by-name for
   globals; `this`/`super` are synthetic locals in scopes wrapping methods.
3. **Evaluate** by walking the tree: environments are parent-linked slot
   frames; globals are a hash table; `return` unwinds via a flag; closures
   capture their frame; bound methods are closures with a one-slot `this`
   frame.

## 5. Test contract

A conforming implementation must satisfy the embedded `// expect:` /
`// expect runtime error:` annotations in `test/cases/*.lox` (see
[testing.md](testing.md)), runnable against any implementation via
`ANLOX=/path/to/impl ruby test/run_tests.rb`.  Because Lox has no canonical
system binary, the expected output lives in the fixtures themselves; an
optional `ANLOX_REF` adds a diff against a reference `jlox`/`clox`.
