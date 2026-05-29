# AnLox — implemented

## Language (Crafting Interpreters' Lox, jlox level)

- Values: `nil`, booleans, numbers (double, boxed), strings, closures, classes,
  instances, native functions.
- Literals; `( )` grouping; `print` and expression statements.
- Arithmetic `+ - * /` and unary `-` (numbers); `+` overloaded for string
  concatenation; comparisons `< <= > >=`; equality `== !=`; `!`; `and` / `or`
  (short-circuit, operand-returning).
- `var` declarations (block-scoped locals / late-bound globals), assignment.
- `{ }` blocks with lexical scoping & shadowing.
- `if` / `else`, `while`, `for` (desugared to a scoped while).
- Functions (`fun`), first-class closures capturing their scope, recursion,
  `return`, arity checking.
- Classes: methods, dynamic instance fields, `this`, `init` constructor,
  single inheritance (`class B < A`), `super`, bound methods, dynamic dispatch.
- Native `clock()`.
- Runtime errors with the book's messages (undefined variable, operand type,
  arity, property access); exit codes 65 (compile) / 70 (runtime).
- Resolver static checks: use-before-define in initializer, duplicate local,
  `return`/`this`/`super` misuse, self-inheritance.

## ASTro framework integration

- 38 AST nodes; children via `EVAL_ARG` so the SD specializer folds the chain.
- Resolver maps locals to `(depth, slot)` at parse time (`node_local`); globals
  by name; `this`/`super` as locals.
- Variable-arity children (statement lists, call args, method sets) in
  GC-allocated side-tables; each runtime-dispatched node is a code-store entry.
- `--aot-compile` bakes the program, every function body, and every side-table
  child into specialized dispatchers (`code_store/all.so`).

## Tooling

- `test/run_tests.rb` — Crafting Interpreters' `// expect:` annotated format
  (self-contained), with optional `ANLOX_REF` differential testing; 9 fixtures.
- `benchmark/run_bench.rb` — interp vs AOT.
- `docs/` — spec, reimplementation reference, testing contract, runtime, perf,
  done/todo.
