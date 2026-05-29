# Testing AnCaml (and any other MinCaml implementation)

`ancaml`'s correctness is checked by **differential testing against the system
OCaml**, exploiting that MinCaml is (almost) a subset of OCaml.  The harness
is implementation-agnostic: point it at any MinCaml interpreter/compiler via
the `ANCAML` environment variable and it will hold that implementation to the
same contract.

```sh
make check                       # uses ./ancaml and `ocaml`
ANCAML=/path/to/other ruby test/run_tests.rb
OCAML=/path/to/ocaml ruby test/run_tests.rb
```

## The MinCaml ↔ OCaml prelude

A MinCaml program is run under OCaml by prepending two lines and wrapping the
program as a `let _ = ( … )` binding:

```ocaml
let print_char n = print_char (Char.chr n)         (* MinCaml print_char : int -> unit *)
module Array = struct include Stdlib.Array let create = make end  (* Array.create was removed *)
let _ = (
  <program>
)
```

These are the *only* two surface reconciliations needed.  Everything else —
`let rec`, tuples, `a.(i) <- v`, structural `=`/`<=`, the float operators,
`print_int` / `sqrt` / `truncate` / … — is shared verbatim, so the two
implementations must produce byte-identical stdout.

## Test groups (`test/run_tests.rb`)

1. **positive** — a well-typed program; `ancaml`'s stdout must equal `ocaml`'s.
2. **type-error** — a program `ancaml` must *statically reject* (non-zero exit).
   The same text is usually accepted by OCaml, which demonstrates that the
   only difference is MinCaml's stricter static discipline (e.g. no integer
   `*`, no mixed int/float arithmetic, `;` requires a `unit` left operand,
   exact-arity application, unbound variables).
3. **fixtures** — every `test/cases/*.ml`, compared against OCaml.

## Writing portable test programs

To keep `ancaml`-vs-`ocaml` output identical:

- Keep integer results within 32-bit range (literals are 32-bit; see
  [spec.md](spec.md)).  Intermediate 63-bit values are fine, final printed
  ints should match OCaml's 63-bit `int` anyway for in-range values.
- Prefer printing **ints** (`print_int`).  For floats, scale and `truncate`
  to an int rather than relying on `print_float` formatting unless you have
  verified both sides format identically.
- Don't use integer `*` / `/` (MinCaml has neither) — use `+ -`, or compute
  in `float` with `*.` `/.`.
- Avoid extremely deep *tail* recursion: `ancaml` does not yet do tail-call
  elimination (see [todo.md](todo.md)), so a multi-million-iteration tail
  loop overflows the C stack.  Tree-recursive programs (fib/ack/…) of
  bounded depth are fine.

## Adding a fixture

Drop a `.ml` file in `test/cases/`.  It is run by both `ancaml` and `ocaml`
(with the prelude) and the outputs are compared.  Keep comments free of a
stray `*)` (it closes the comment early — in `ancaml` *and* OCaml).
