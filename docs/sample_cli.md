# ASTro Sample CLI Reference

Cross-sample CLI option survey + porting status.

This document is the single source of truth for "which sample is on
the new framework CLI" and "what mapping each pending sample needs."
Updated 2026-06-18 (added the canonical `--compiled-only` mode).

> **Status of `--compiled-only`:** framework flag `bcfg.compiled_only`
> (`runtime/astro_build.{c,h}`) **implemented**; **koruby_precise wired**
> (post-swap poison of unswapped `code_repo` bodies + program root → exit 7
> with the body name on the first interpreter dispatch).  Other standalone
> samples still need the one-line `if (bcfg.compiled_only) …` wiring
> (abruby has its own standalone `--compiled-only`).

## TL;DR — the canonical CLI

The framework (`runtime/astro_build.{c,h}`) now owns and parses these
flags before the sample's own parser ever sees them.  See
[`usage.md`](./usage.md) "Framework-owned CLI" for the full design.

Order-free, canonical-only (no aliases):

| flag | bcfg field | meaning |
|---|---|---|
| `--plain` | `bcfg.plain` | pure interpreter, ignore compiled code |
| `--compiled-only` | `bcfg.compiled_only` | strict inverse of `--plain`: run only baked SDs; abort if any default (interpreter) dispatcher is reached — AOT **compile-miss detection** (see note below) |
| `--aot-compile` | `bcfg.aot_compile` | bake AOT specializations |
| `--pg-compile` | `bcfg.pg_compile` | bake PG specializations (implies `--run`) |
| `--run` | `bcfg.run` | execute (default in runtime, opt-in for build) |
| `--build OUT` | `bcfg.out_exe` | emit a standalone exe at OUT |
| `-q` / `--quiet` | `bcfg.quiet` | translate to sample's quiet state |
| `-v` / `--verbose` | `bcfg.verbose` | translate to sample's verbose state |
| `-h` / `--help` | `bcfg.help_requested` | signal — sample calls usage() and exits |
| `--version` | `bcfg.version_requested` | signal — sample prints `ASTRO_VERSION` and exits |

C-toolchain knobs (`--cc`, `-O*`, `--strip`, `--lto`, `--gc-sections`,
etc.) are in the `ASTRO_BUILD_OPTS` env var, not argv.

### `--compiled-only` (compile-miss detection)

The three execution modes form a line:

| mode | flag | interpreter | compiled SDs |
|---|---|---|---|
| interp-only | `--plain` | always | never |
| hybrid (default) | *(none)* | fallback when no SD | when baked & matched |
| compiled-only | `--compiled-only` | **never (abort)** | required |

`--compiled-only` is a **debugging** mode: it makes a silent AOT
compile-miss loud.  In hybrid mode a body whose bake was skipped (hash
mismatch, an entry never registered, a not-yet-specializable node) just
falls back to the interpreter and runs correctly but slowly — invisible
in a benchmark.  Under `--compiled-only` every non-swapped body's
dispatcher is replaced with a poison stub, so the first such body to run
aborts with its node kind + source location, pinpointing the gap.

Typical use: `<prog> --aot-compile FILE` then `<prog> --compiled-only FILE`
(or the bench harness's compiled-only runner) to *prove* the AOT covers
the whole program.  abruby already ships this as `--compiled-only`
("abort if default dispatcher is used"); the framework adopts the same
name so every sample's compiled-only runner is spelled identically.

Implementation note: abruby NULLs every node's dispatcher at allocation
(`OPTION.compiled_only ? NULL : DISPATCH_node_X`) and lets the swap fill
the SDs, so any unfilled node crashes.  Samples whose SDs are per-body
(koruby_precise etc.) can instead post-pass after the swap: set each
unswapped `code_repo` body (+ program root) to a poison dispatcher —
cheaper and gives a clean diagnostic instead of a raw NULL deref.

## Port status

| Sample | Status | Notes |
|---|---|---|
| calc | ✅ done | `--no-compile` → framework `--plain`.  See [sample/calc/main.c](../sample/calc/main.c). |
| naruby | ✅ done | Old `-i`/`-c`/`-p`/`--aot`/`--aot-compile-first`/`--pg`/etc. removed.  Sample-only flags kept: `--ccs`/`--clear-code-store`, `-s`, `-b`, `-j`.  See [sample/naruby/main.c](../sample/naruby/main.c) + [sample/naruby/naruby_parse.c](../sample/naruby/naruby_parse.c). |
| koruby | ✅ done | Old `-c`/`--aot-compile` (alone)/`-q`/`-v` removed.  `--dump` renamed to `--dump-ast`.  Dead `node_specialized.c` / `compiled_koruby` machinery deleted.  See [sample/koruby/main.c](../sample/koruby/main.c). |
| baruby | ⏳ pending | naruby fork — same flag set as naruby; same mapping should apply. |
| baruby_precise | ✅ done | naruby-pattern port; `-i`/`-c`/`-p`/`--aot`/`--aot-compile-first`/`--pg` removed, sample-specific `--ccs`/`-s`/`-b`/`-j` kept.  See [sample/baruby_precise/main.c](../sample/baruby_precise/main.c). |
| abruby | n/a | CRuby C extension, no standalone CLI. |
| arjsv | n/a | CRuby C extension. |
| jstro | ⏳ pending | `--no-compile`, `-c`/`--aot-compile-first`, `--aot-compile` (alone), `-p`/`--pg-compile`, `--dump`, `-q`, `-v`, `-h` — all to remove. |
| luastro | ⏳ pending | `--no-compile`, `-c`/`--aot-compile-first`, `--aot-compile`, `-p`/`--pg-compile`, `-q`, `-v`, `-h`, `--help`. |
| pystro | ⏳ pending | `--no-compile`, `-c`, `--aot-compile`, `-q`, `-h`, `--help`. |
| castro | ⏳ pending | `--no-compile`, `-c`/`--compile-all`, `--dump`, `-q`/`--quiet`. |
| asom | ⏳ pending | `--plain`, `-c`/`--aot-compile-first`, `-p`/`--pg`/`--pg-compile`, `--dump-ast`, `-q`/`--quiet`, `--verbose`. |
| ascheme | ✅ done | koruby-pattern port (AOT always implies run); `-c`/`--compile`/`--pg`/`-q`/`-v`/`-h` removed, sample-specific `-e`/`-`/`--clear-cs` kept.  See [sample/ascheme/main.c](../sample/ascheme/main.c). |
| ascheme_precise | ✅ done | koruby-pattern port (AOT always implies run); `-c`/`--compile`/`--pg`/`-q`/`-v`/`-h` removed, sample-specific `-e`/`-`/`--clear-cs` kept.  See [sample/ascheme_precise/main.c](../sample/ascheme_precise/main.c). |
| astocaml | ⏳ pending | `-c`/`--compile`, `--no-compile`, `-q`/`--quiet`. |
| asml | ⏳ pending | `-c`/`--compile`, `--no-compile`, `-e`, `-q`/`--quiet`, `-h`/`--help`. |
| astr | ⏳ pending | `--plain`/`-i`, `-c`/`--aot`, `--ccs`, `-q`/`--quiet`. |
| wastro | ⏳ pending | `--no-compile`, `-c` (compile_first), `-q`/`--quiet`, `--clear-cs`/`--ccs`, `-v`/`--verbose`. |
| pascalast | ⏳ pending | `--no-compile`, `-c`, `-q`/`--quiet`. |
| aforth | ⏳ pending | `-c`/`--aot-compile`, `--no-compile`, `--dump-ast`, `-q`, `-h`/`--help`. |
| arawk | ⏳ pending | `-c`/`--aot` + `--aot-compile`, `--plain`/`-i`, `-f file`, `--ccs`, `--dump-ast`. |
| arcel | ⏳ pending | `--no-compile`, `--compile`. |
| astrogre | ⏳ pending | `--aot`, `-q`, `-e`, `-V`/`--version` (already!).  `are` CLI uses these. |
| nuq | ⏳ pending | `--no-compile`, mostly jq-style flags otherwise. |

## Porting recipe

For each pending sample:

### 1. Update `main.c` argv handling

At the very top of `main()`, call the framework:

```c
struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;

if (bcfg.help_requested)    { usage(); return 0; }
if (bcfg.version_requested) { printf("<prog> " ASTRO_VERSION "\n"); return 0; }

/* Translate bcfg → sample's OPTION struct (mapping is per-sample). */
if (bcfg.quiet)       OPTION.quiet           = true;
if (bcfg.verbose)     OPTION.verbose         = true;
if (bcfg.plain)       OPTION.no_compiled_code = true;   /* or sample's "skip AOT" flag */
if (bcfg.compiled_only) OPTION.compiled_only  = true;  /* poison non-swapped dispatchers (compile-miss detect) */
if (bcfg.aot_compile) OPTION.aot_compile     = true;   /* or per-sample mapping */
if (bcfg.pg_compile)  OPTION.pg_compile      = true;
/* run / out_exe are inspected later when dispatching to build vs run */
```

Add `#include "../../runtime/astro_build.h"` near the other runtime
includes.

### 2. Strip the sample's old parser

Delete from the sample's own option parser **every** spelling of the
flags the framework now owns (canonical names AND aliases):

| concept | old spellings to remove |
|---|---|
| pure interpreter | `--no-compile`, `--plain`, `-i` |
| AOT bake before run | `-c`, `--aot-compile-first`, `--aot` (as "compile-then-run"), `--compile`, `--compile-all`, `--aot-compile` (when overloaded) |
| AOT bake only, no run | `--aot-compile` (the "bake-only" sense) |
| PG bake | `-p`, `--pg`, `--pg-compile` |
| quiet | `-q`, `--quiet` |
| verbose | `-v`, `--verbose` |
| help | `-h`, `--help` |
| version | `-V`, `--version` |

Sample-specific flags (`-e`, `--ccs`, `--dump-ast`, `-j` JIT, language
parser switches) stay.  Sample-specific aliases stay too — the
no-alias rule is only for framework flags.

### 3. Decide the AOT semantics for this sample

Two patterns observed:

- **koruby pattern** — language must run to discover method ASTs.
  `--aot-compile` always implies a run; `bcfg.run` carries no extra
  information.  Mapping: `if (bcfg.aot_compile) g_aot_compile = true;`
  unconditionally.  Use this for samples whose AOT bake depends on a
  populated `code_repo`.

- **naruby pattern** — bake-only is meaningful.  Mapping:
  `if (bcfg.aot_compile && !bcfg.run) OPTION.compile_only = true;`
  + `if (bcfg.aot_compile && bcfg.run) OPTION.compile_first = true;`
  Use this when the sample can bake without running (e.g. parses-time
  discovers all entries).

Pick whichever fits the sample's existing AOT pipeline; do not refactor
the bake itself.

### 4. Update `usage()` / `show_help()`

Sample's help text should only mention sample-specific flags, then
delegate to the framework:

```c
static void usage(void) {
    fprintf(stderr,
        "usage: <prog> [options] [file] [argv...]\n"
        "\n"
        "<prog>-specific options:\n"
        "  -e <code>          eval code\n"
        "      --dump-ast     dump the parsed AST and exit\n"
        "\n");
    astro_print_build_help(stderr);
    exit(1);
}
```

### 5. Update bench scripts / Makefile / docs

Search for the old flag names and replace:

```sh
grep -rn -E '(-i|-c|-p)\b|--no-compile|--aot-compile-first|--aot\b' \
    sample/<lang>/ --include='*.sh' --include='Makefile' --include='*.md' \
    --include='*.rb'
```

Translation table:

| old | new |
|---|---|
| `-i` / `--no-compile` | `--plain` |
| `-c` (= "compile-then-run") | `--aot-compile --run` (naruby pattern) or `--aot-compile` (koruby pattern) |
| `--aot-compile-first` / `--aot` | same as `-c` above |
| `-c` (= "compile_only" old sense) | `--aot-compile` (no `--run`) |
| `-p` / `--pg` / `--pg-compile` | `--pg-compile` |
| `-q` | `--quiet` (or keep `-q` since framework accepts both) |
| `--dump` | `--dump-ast` (preferred — pick once per sample if alias collision) |

### 6. Smoke test

Build + run all 25-ish tests (sample-specific path).  Compare
warnings before/after — should be ≤ pre-existing count.

```sh
cd sample/<lang>
make clean && make 2>&1 | grep -c 'warning:'   # baseline
# … apply edits …
make clean && make 2>&1 | grep -c 'warning:'   # must be ≤ baseline
```

If the sample has bench scripts, run one with the new flag spelling
to confirm.

## Reference: pre-port spellings (snapshot 2026-05-22)

The matrix below is for historical reference — what each sample
accepted BEFORE the port.  After porting a sample, replace its row
with "see canonical CLI above" or delete the row entirely.

A blank cell means the sample didn't expose that knob.  Canonical
spelling first, aliases in parens.

| concept | jstro | luastro | pystro | castro | asom | ascheme | astocaml | asml | astr | wastro | pascalast | aforth | arawk | arcel | astrogre | nuq |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| source: file (positional) | ✓ | ✓ | ✓ | ✓ | (class name) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ (`-f`) | (subcmd) | (subcmd) | (filter+file) |
| source: `-e EXPR` | — | `-e` | `-e` | — | — | `-e` | — | `-e` | — | — | — | — | `-e` | `eval -e` | `-e` | — |
| quiet | `-q` | `-q` | `-q` | `-q` `--quiet` | `-q` `--quiet` | `-q` `--quiet` | `-q` `--quiet` | `-q` `--quiet` | `-q` `--quiet` | `-q` `--quiet` | `-q` `--quiet` | `-q` | (n/a) | `-q` `--quiet` | `-q` | `--quiet` |
| verbose | `-v` | `-v` | — | — | `--verbose` | `-v` `--verbose` | — | — | — | `-v` `--verbose` | — | — | — | — | — | — |
| interp only, no AOT | `--no-compile` | `--no-compile` | `--no-compile` | `--no-compile` | `--plain` | (default) | `--no-compile` | `--no-compile` | `--plain` `-i` | `--no-compile` | `--no-compile` | `--no-compile` | `--plain` `-i` | `--no-compile` | — | `--no-compile` |
| AOT-bake before run | `-c` `--aot-compile-first` | `-c` `--aot-compile-first` | `-c` | `-c` `--compile-all` | `-c` `--aot-compile-first` | `-c` `--compile` | `-c` `--compile` | `-c` `--compile` | `-c` `--aot` | `-c` (compile_first) | `-c` | `--aot-compile` | `-c` `--aot` | `--compile` | `--aot` | — |
| AOT-bake only, no run | `--aot-compile` | `--aot-compile` | `--aot-compile` | — | — | — | — | — | — | — | — | — | `--aot-compile` | — | — | — |
| PG-bake after run | `-p` `--pg-compile` | `-p` `--pg-compile` | — | — | `-p` `--pg` `--pg-compile` | `--pg-compile` `--pg` | — | — | — | — | — | — | — | — | — | — |
| clear code store | — | — | — | — | — | `--clear-cs` | — | — | `--ccs` | `--clear-cs` `--ccs` | — | — | `--ccs` | — | — | — |
| dump AST | `--dump` | `--dump-ast` | `--dump-ast` | `--dump` | `--dump-ast` | (probably) | (probably) | (probably) | `--dump-ast` | (probably) | `--dump-ast` | `--dump-ast` | `--dump-ast` | — | — | — |
| help | (no flag) | `-h` `--help` | `-h` `--help` | (no flag) | (no flag) | `-h` `--help` | (probably) | `-h` `--help` | (no flag) | `-h` `--help` | (no flag) | `-h` `--help` | (no flag) | (no flag) | `-V` `--version` | — |

Aliases that don't fit anywhere uniform (sample-specific, untouched
by the port):

- naruby `-s` (source map output), `-b` (skip bake), `-j` (JIT),
  `--ccs` (clear code store)
- wastro `--aot` (skip PGC lookup — different meaning!)
- astrogre `--aot` (AOT-bake patterns, also different meaning)
- arawk `-f file` (POSIX awk convention)
- jq-style: `nuq` short flags (`-r`, `-R`, `-c`, `-s`, etc.)

## Samples without a CLI

- **abruby**, **arjsv** — CRuby C extensions loaded via `ruby -r`.
  No standalone `main()`, nothing to port.
- **astrogre** — has its own `are` grep CLI (separate from the
  embedder library); listed above.
