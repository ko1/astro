# ASTro Sample CLI Reference

Single source of truth for the cross-sample command-line interface:

1. **[Part 1 — the canonical CLI](#part-1--the-canonical-cli)** — the
   framework-owned flags and their *exact* meaning (the contract every
   sample must obey).
2. **[Part 2 — implementation examples](#part-2--implementation-examples)** —
   how that contract is wired into a sample (`main.c` plumbing, build
   mode, the porting recipe).
3. **[Part 3 — per-sample status](#part-3--per-sample-status)** — where
   each sample currently stands, and the intent to converge them all on
   Part 1.

Updated 2026-06-18: consolidated the CLI design out of `usage.md`, and
**fixed the run convention** — `--aot-compile` is a *compile* option and
does **not** execute on its own (see
[the run convention](#examples-and-the-run-convention)).

---

## Part 1 — the canonical CLI

The framework (`runtime/astro_build.{c,h}`) owns and parses these flags
*before* the sample's own parser sees them: `astro_build_extract_flags`
pulls them out of argv in place and hands the residual argv to the
sample.  Source files are positional; parsing stops at the first
non-`-` token (Unix convention — everything after it is the running
program's ARGV).

So every sample's CLI is two layers:

- **Framework layer** — flags whose meaning is identical across all
  samples (the mode flags below, build output, quiet/verbose/help/version).
- **Sample layer** — anything sample-specific (`-e EXPR`, parser
  switches, dump variants, JIT toggles, …).

### Flags at a glance

Order-free, canonical-only (no aliases):

| flag | meaning |
|---|---|
| `--plain` | bake nothing & ignore compiled code (pure AST-walk) |
| `--aot-compile` | bake AOT specializations (does **not** run — add `--run`) |
| `--pg-compile` | bake profile-guided specializations (implies `--run`) |
| `--compiled-only` | run-time: use only baked SDs; abort on interpreter dispatch (compile-miss detection) |
| `--run` | execute (default for a plain invocation; **required** under `--aot-compile` / `--build`) |
| `--build OUT` | build mode: emit a standalone exe at OUT |
| `-q` / `--quiet` | translate to the sample's quiet state |
| `-v` / `--verbose` | translate to the sample's verbose state |
| `-h` / `--help` | sample prints its help and exits |
| `--version` | sample prints `ASTRO_VERSION` and exits |

C-toolchain knobs (`--cc`, `-O*`, `--strip`, `--lto`, `--gc-sections`,
…) live in the `ASTRO_BUILD_OPTS` env var, not argv (see
[§ ASTRO_BUILD_OPTS](#astro_build_opts-env-var)).

The rest of Part 1 explains the compile/run model behind this table.

### Compile & run modes

The mode flags — what (if anything) to bake, and whether/where to run.
They are the heart of the CLI; everything below details them.

#### Two axes: *attribute* (what to bake) × *action* (whether to run)

The mode flags fall on two orthogonal axes: **attribute** — *what kind of
compiled code to bake* — and **action** — *whether/where to run the
program*.  Pick one from each (the defaults are "bake nothing" and "run").

**attribute** — *what kind of compiled code to bake* (what goes into the
exe / `code_store/`):

| flag | meaning |
|---|---|
| *(none)* | bake nothing; at runtime, auto-load an existing `code_store/all.so` if present (hybrid) |
| `--plain` | bake nothing **and** ignore any existing compiled code (pure AST-walk) |
| `--aot-compile` | bake AOT-specialized dispatchers |
| `--pg-compile` | bake profile-guided specializations |

**action** — *whether/where to run the program*:

| flag | meaning |
|---|---|
| *(none)* | execute (the default — see the run convention below) |
| `--run` | force execution |
| `--build OUT` | build mode: emit a standalone executable at OUT |

On top of these, **`--compiled-only`** is a
separate *run-time* strictness mode — the strict inverse of `--plain`:
use **only** baked SDs and abort on any interpreter dispatch (AOT
compile-miss detection; see
[§ --compiled-only](#--compiled-only-compile-miss-detection)).  It is not
one of the three attribute choices above — it modifies how a run *uses*
compiled code, not what gets baked.

#### Examples (and the run convention)

**The run convention:** a flag that *produces an artifact*
(`--aot-compile`, `--build`) does **not** run on its own — add `--run`.
Everything else runs: a plain invocation by default, and `--pg-compile` /
`--compiled-only` by nature (`--pg-compile` must run, to profile).  The
tables below show every combination.

> Canonical behaviour = the "naruby pattern" below.  A few samples still
> always-run on `--aot-compile` (the older "koruby pattern", no bake-only
> mode) — deviations to be aligned, see [Part 3](#part-3--per-sample-status).

**Runtime mode** (no `--build` — bakes into / auto-loads `code_store/`;
this is how most samples are used):

| command | bakes | runs? |
|---|---|---|
| `naruby main.rb` | — | yes (hybrid: uses `code_store/all.so` if present) |
| `naruby --plain main.rb` | — | yes (pure interpreter) |
| `naruby --aot-compile main.rb` | AOT → `code_store/` | **no** |
| `naruby --aot-compile --run main.rb` | AOT → `code_store/` | yes |
| `naruby --pg-compile main.rb` | PG → `code_store/` | yes (profile) |
| `naruby --compiled-only main.rb` | — | yes (only baked SDs; abort on interp dispatch) |

**Build mode** (`--build OUT` — bundles into a standalone exe):

| command | exe content | runs during build? |
|---|---|---|
| `naruby --build out main.rb` | AST only | no |
| `naruby --build out --aot-compile main.rb` | AOT SDs (main entry only) | no |
| `naruby --build out --aot-compile --run main.rb` | AOT SDs (auto file set via run) | yes |
| `naruby --build out --pg-compile main.rb` | AOT + PG SDs | yes (profile) |
| `naruby --build out main.rb arg1.rb arg2.rb` | AST of all three | no |

Size-vs-speed trade-off (koruby fib(35)):

| mode | exe size | run time |
|---|---|---|
| `--build out` (default) | 1.0 MB | 0.61 s |
| `--build out --aot-compile` | 1.0 MB | 0.59 s |
| `--build out --pg-compile` | 4.3 MB | 0.36 s |

Note: bake-only (`--aot-compile` without `--run`) bakes only what is
statically reachable — typically just the main entry; adding `--run` lets
the bake discover the full set (method bodies register as their `def`s
execute).

#### `--compiled-only` (compile-miss detection)

The three execution modes form a line:

| mode | flag | interpreter | compiled SDs |
|---|---|---|---|
| interp-only | `--plain` | always | never |
| hybrid (default) | *(none)* | fallback when no SD | when baked & matched |
| compiled-only | `--compiled-only` | **never (abort)** | required |

A **debugging** mode that makes a silent AOT compile-miss loud: it runs
only baked SDs and aborts the moment an interpreter dispatcher would run,
naming the offending node (kind + source location).  In hybrid mode a
skipped-bake body (hash mismatch, unregistered entry, not-yet-
specializable node) would instead fall back to the interpreter and run
correctly but slowly — invisible in a benchmark.  Typical use:
`<prog> --aot-compile --run FILE` then `<prog> --compiled-only FILE` to
prove the AOT covers the whole program.  (Wiring it into a sample + the
`@noinline` caveat: see [Part 2](#wiring---compiled-only).)

### Universal CLI knobs

The framework also owns and parses (no aliases, canonical only):

| flag | sample treatment |
|---|---|
| `-q` / `--quiet` | translate to the sample's own quiet state |
| `-v` / `--verbose` | translate to the sample's own verbose state |
| `-h` / `--help` | signal: sample prints its own help and exits |
| `--version` | signal: sample prints version and exits |

**Bare `-v` rule:** `astro_build_extract_flags` treats a lone `-v` /
`--verbose` — given with *no other arguments* (no files, no sample
flags, no `--build`) — as a version request, so `prog -v` prints the
version and exits.  Combined with real work
(`prog -v foo`, `prog -v -e ...`) it keeps its "verbose" meaning.
Framework-wide, so every sample behaves the same.

Sample-specific knobs may add their own short aliases (e.g. naruby `-j`
for JIT) but **the framework flags above have no aliases** — this
guarantees `--quiet` etc. mean exactly the same thing everywhere.

### `ASTRO_BUILD_OPTS` env var

C-toolchain knobs (CC, optimization, strip, lto, …) live in the
`ASTRO_BUILD_OPTS` environment variable, NOT in argv.  This keeps argv
purely "what to do" and env "how to compile":

```sh
ASTRO_BUILD_OPTS="--cc=clang -O3 --strip --gc-sections" \
    naruby --build out main.rb
```

Whitespace-separated tokens:

| token | effect |
|---|---|
| `--cc=PATH` | C compiler (default: `$ASTRO_CC` → `$CC` → `cc`) |
| `-O0`/`-O1`/`-O2`/`-O3`/`-Os`/`-Og`, `--opt=N` | Optimization level |
| `--debug` / `--no-debug` | `-ggdb3` |
| `--strip` / `--no-strip` | Post-link `strip` |
| `--lto` / `--no-lto` | `-flto` |
| `--static` | `-static` |
| `--gc-sections` | `-ffunction-sections -fdata-sections -Wl,--gc-sections` |
| `--sanitize=LIST` | `-fsanitize=LIST` |
| `--cflag=ARG` | Pass-through compile flag (repeatable) |
| `--ldflag=ARG` | Pass-through linker flag (repeatable) |
| `--show-cmd` | Print the cc command line |
| `--keep` | Don't unlink `_embed.c` |

---

## Part 2 — implementation examples

### Positional argument handling

The interpretation of positional args depends on whether the program
runs (per [the run convention](#examples-and-the-run-convention)):

- **runs**: first positional = entry source, rest = ARGV for the run
- **doesn't run**: positionals = source file list to embed (first = entry)

So `naruby --build out --run foo.rb arg1` runs foo.rb with ARGV=[arg1].
`naruby --build out foo.rb bar.rb` embeds both files (no run).

### Wiring a new sample

Five touch points (see `sample/calc/main.c` for the smallest worked
example):

1. **`exe_main.c`** — minimal driver, runs alongside the sample's
   regular `main.c`.  The framework has pre-baked the dispatcher
   pointers into the embedded AST, so no `astro_cs_*` calls are needed
   at exe runtime.  Skeleton (~10 lines for simple samples):

   ```c
   #include "context.h"
   #include "node.h"

   struct yourlang_option OPTION;
   extern NODE *astro_build_embedded_ast(void);

   int main(int argc, char *argv[]) {
       (void)argc; (void)argv;
       CTX *c = make_context();        // your usual init
       EVAL(c, astro_build_embedded_ast());
       return 0;
   }
   ```

2. **`main.c` plumbing** — at the very start of `main()`:

   ```c
   int main(int argc, char *argv[]) {
       struct astro_build_config bcfg = ASTRO_BUILD_CONFIG_INIT;
       if (astro_build_extract_flags(&argc, argv, &bcfg) != 0) return 1;

       /* Signals the framework set; we act on them. */
       if (bcfg.help_requested)    { usage(); return 0; }
       if (bcfg.version_requested) { printf("mylang " ASTRO_VERSION "\n"); return 0; }

       /* Translate framework bcfg → sample's OPTION struct (per-sample). */
       if (bcfg.quiet)         OPTION.quiet = true;
       if (bcfg.verbose)       OPTION.verbose = true;
       if (bcfg.plain)         OPTION.no_compiled_code = true;
       if (bcfg.compiled_only) OPTION.compiled_only = true;
       if (bcfg.pg_compile)    OPTION.pg_compile = true;

       /* AOT + the run convention (canonical / naruby pattern): */
       if (bcfg.aot_compile && !bcfg.run) OPTION.compile_only  = true; /* bake, don't run */
       if (bcfg.aot_compile &&  bcfg.run) OPTION.compile_first = true; /* bake, then run */

       /* … existing sample-specific parser walks the remaining argv … */

       /* Build mode: dispatch to astro_build_aot_executable. */
       if (bcfg.out_exe) {
           astro_build_begin_aot_session();
           if (bcfg.aot_compile || bcfg.pg_compile) {
               astro_cs_compile(ast, NULL);
               /* iterate code_repo and astro_cs_compile each method body */
           }
           bcfg.src_dir = MYLANG_SRC_DIR;
           bcfg.runtime_dir = ASTRO_RUNTIME_DIR;
           static const char *sources[] = { "parse.c", "node.c", "exe_main.c", NULL };
           bcfg.sources = sources;
           int rc = astro_build_aot_executable(ast, &bcfg, "code_store");
           astro_build_end_aot_session();
           astro_build_config_dispose(&bcfg);
           return rc;
       }
       /* … normal runtime path: run the AST (unless compile_only) … */
   }
   ```

3. **Sample's own option parser** — DELETE anything that duplicates a
   framework flag: no per-sample `--plain`, `--aot-compile`,
   `--aot-compile-first`, `--pg-compile`, `-c`, `-p`, `-i`,
   `--no-compile`, `-q`, `--quiet`, `-v`, `--verbose`, `-h`, `--help`,
   `--version`.  Aliases of these were deliberately removed — the
   framework is the single source of truth.  Sample's `usage()` should
   end with `astro_print_build_help(stderr)`.

4. **`node.c`** — add `#include "node_emit_ast.c"` (auto-generated from
   `node.def`) BEFORE `#include "node_alloc.c"`, and
   `#include "astro_build.c"` after `#include "astro_code_store.c"`.

5. **`Makefile`** — `-DLANG_SRC_DIR='"$(abspath .)"'` and
   `-DASTRO_RUNTIME_DIR='"$(abspath $(RUNTIME))"'` so the host knows
   where to find its sources at exe-build time.

### Wiring `--compiled-only`

Two implementations.  abruby NULLs every node's dispatcher at allocation
(`OPTION.compiled_only ? NULL : DISPATCH_node_X`) and lets the swap fill
the SDs, so any unfilled node crashes.  Samples with per-body SDs
(koruby_precise etc.) instead post-pass *after* the swap: point each
unswapped `code_repo` body (+ the program root) at a poison dispatcher —
cheaper, and a clean diagnostic (node kind + location) instead of a raw
NULL deref.

**Compile-exempt bodies.**  A body whose root is an `@noinline` node (a
method/lambda that is *just* a `node_make_proc` / `node_class` /
`node_module`) carries a per-process `NODE*` entry operand the SD
machinery can't bake as a literal — that needs reload-time operand fixup,
not implemented (see `project_astro_value_consts_gap`).  Such bodies
legitimately run on the interpreter, so the poison pass must **skip
`head.flags.no_inline` body roots** — otherwise every program with a
class or proc false-positives.

### Porting recipe (existing sample)

#### 1. Update `main.c` argv handling

At the very top of `main()`, call the framework (see the plumbing
skeleton above), then translate `bcfg` → the sample's `OPTION` struct.
Add `#include "../../runtime/astro_build.h"` near the other runtime
includes.

#### 2. Strip the sample's old parser

Delete from the sample's own option parser **every** spelling of the
flags the framework now owns (canonical names AND aliases):

| concept | old spellings to remove |
|---|---|
| pure interpreter | `--no-compile`, `--plain`, `-i` |
| AOT bake before run | `-c`, `--aot-compile-first`, `--aot` (as "compile-then-run"), `--compile`, `--compile-all` |
| AOT bake only, no run | `--aot-compile` (the "bake-only" sense) |
| PG bake | `-p`, `--pg`, `--pg-compile` |
| quiet / verbose / help / version | `-q`/`--quiet`, `-v`/`--verbose`, `-h`/`--help`, `-V`/`--version` |

Sample-specific flags (`-e`, `--ccs`, `--dump-ast`, `-j` JIT, parser
switches) stay; sample-specific aliases stay too — the no-alias rule is
only for framework flags.

#### 3. Apply the run convention

Map AOT to the canonical (naruby) pattern:

```c
if (bcfg.aot_compile && !bcfg.run) OPTION.compile_only  = true; /* bake only */
if (bcfg.aot_compile &&  bcfg.run) OPTION.compile_first = true; /* bake then run */
```

If the sample's bake set is discovered by running (method ASTs
registered as `def`s execute), bake-only legitimately covers only the
main entry; `--run` lets it see the full reachable set.  Do **not**
re-adopt the old "always run on `--aot-compile`" shortcut.

#### 4. Update `usage()` / `show_help()`

```c
static void usage(void) {
    fprintf(stderr,
        "usage: <prog> [options] [file] [argv...]\n\n"
        "<prog>-specific options:\n"
        "  -e <code>          eval code\n"
        "      --dump-ast     dump the parsed AST and exit\n\n");
    astro_print_build_help(stderr);
    exit(1);
}
```

#### 5. Update bench scripts / Makefile / docs

```sh
grep -rn -E '(-i|-c|-p)\b|--no-compile|--aot-compile-first|--aot\b' \
    sample/<lang>/ --include='*.sh' --include='Makefile' --include='*.md' \
    --include='*.rb'
```

Translation table:

| old | new |
|---|---|
| `-i` / `--no-compile` | `--plain` |
| `-c` (= "compile-then-run") / `--aot-compile-first` / `--aot` | `--aot-compile --run` |
| `-c` (= "compile_only" old sense) | `--aot-compile` (no `--run`) |
| `-p` / `--pg` / `--pg-compile` | `--pg-compile` |
| `--dump` | `--dump-ast` |

#### 6. Smoke test

```sh
cd sample/<lang>
make clean && make 2>&1 | grep -c 'warning:'   # baseline
# … apply edits …
make clean && make 2>&1 | grep -c 'warning:'   # must be ≤ baseline
```

Run one bench with the new flag spelling to confirm.

---

## Part 3 — per-sample status

Where each sample stands relative to [Part 1](#part-1--the-canonical-cli).
The intent is to converge **every** sample on the canonical CLI,
including the run convention.

### Port status

| Sample | Status | Notes |
|---|---|---|
| calc | ✅ done | `--no-compile` → `--plain`.  [main.c](../sample/calc/main.c). |
| naruby | ✅ done | Canonical run convention (`--aot-compile` bakes only; `--run` to also run).  Old `-i`/`-c`/`-p`/`--aot`/`--aot-compile-first`/`--pg` removed; sample-only `--ccs`/`-s`/`-b`/`-j` kept.  [main.c](../sample/naruby/main.c). |
| koruby | ✅ done | Old `-c`/`--aot-compile` (alone)/`-q`/`-v` removed; `--dump`→`--dump-ast`.  [main.c](../sample/koruby/main.c). |
| baruby | ⏳ pending | naruby fork — same flag set / mapping as naruby. |
| baruby_precise | ✅ done | naruby-pattern (canonical run convention).  [main.c](../sample/baruby_precise/main.c). |
| koruby_precise | ✅ done | Canonical run convention (`--aot-compile` bakes without running — the code repo is populated at *parse* time, so the no-run bake is still complete; `--run` to also run).  `--build` unsupported (M0).  [main.c](../sample/koruby_precise/main.c). |
| **ascheme** | ⚠️ **deviates** | "koruby pattern" — AOT always implies run.  To align to the `--run` convention.  [main.c](../sample/ascheme/main.c). |
| **ascheme_precise** | ⚠️ **deviates** | "koruby pattern" — AOT always implies run.  To align to the `--run` convention.  [main.c](../sample/ascheme_precise/main.c). |
| abruby | n/a | CRuby C extension (ships its own standalone `--compiled-only`). |
| arjsv | n/a | CRuby C extension. |
| jstro | ⏳ pending | remove `--no-compile`, `-c`/`--aot-compile-first`, `--aot-compile` (alone), `-p`/`--pg-compile`, `--dump`, `-q`, `-v`, `-h`. |
| luastro | ⏳ pending | remove `--no-compile`, `-c`/`--aot-compile-first`, `--aot-compile`, `-p`/`--pg-compile`, `-q`, `-v`, `-h`, `--help`. |
| pystro | ⏳ pending | remove `--no-compile`, `-c`, `--aot-compile`, `-q`, `-h`, `--help`. |
| castro | ⏳ pending | remove `--no-compile`, `-c`/`--compile-all`, `--dump`, `-q`/`--quiet`. |
| asom | ⏳ pending | remove `--plain`, `-c`/`--aot-compile-first`, `-p`/`--pg`/`--pg-compile`, `--dump-ast`, `-q`/`--quiet`, `--verbose`. |
| astocaml | ⏳ pending | remove `-c`/`--compile`, `--no-compile`, `-q`/`--quiet`. |
| asml | ⏳ pending | remove `-c`/`--compile`, `--no-compile`, `-e`, `-q`/`--quiet`, `-h`/`--help`. |
| astr | ⏳ pending | remove `--plain`/`-i`, `-c`/`--aot`, `--ccs`, `-q`/`--quiet`. |
| wastro | ⏳ pending | remove `--no-compile`, `-c` (compile_first), `-q`/`--quiet`, `--clear-cs`/`--ccs`, `-v`/`--verbose`. |
| pascalast | ⏳ pending | remove `--no-compile`, `-c`, `-q`/`--quiet`. |
| aforth | ⏳ pending | remove `-c`/`--aot-compile`, `--no-compile`, `--dump-ast`, `-q`, `-h`/`--help`. |
| arawk | ⏳ pending | remove `-c`/`--aot` + `--aot-compile`, `--plain`/`-i`, `--ccs`, `--dump-ast` (keep `-f file`). |
| arcel | ⏳ pending | remove `--no-compile`, `--compile`. |
| astrogre | ⏳ pending | `--aot`, `-q`, `-e`, `-V`/`--version` (the `are` CLI uses these). |
| nuq | ⏳ pending | `--no-compile`; mostly jq-style flags otherwise. |

`--compiled-only` wiring: framework flag `bcfg.compiled_only`
(`runtime/astro_build.{c,h}`) is **implemented**; **koruby_precise is
wired** (post-swap poison of unswapped `code_repo` bodies + program root
→ exit 7 with the body name on the first interpreter dispatch).  Other
standalone samples still need the one-line `if (bcfg.compiled_only) …`
wiring (abruby has its own).

### Reference: pre-port spellings (snapshot 2026-05-22)

For historical reference — what each sample accepted BEFORE the port.
A blank cell means the sample didn't expose that knob; canonical
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

Aliases that don't fit anywhere uniform (sample-specific, untouched by
the port):

- naruby `-s` (source map output), `-b` (skip bake), `-j` (JIT),
  `--ccs` (clear code store)
- wastro `--aot` (skip PGC lookup — different meaning!)
- astrogre `--aot` (AOT-bake patterns, also different meaning)
- arawk `-f file` (POSIX awk convention)
- jq-style: `nuq` short flags (`-r`, `-R`, `-c`, `-s`, etc.)

### Samples without a CLI

- **abruby**, **arjsv** — CRuby C extensions loaded via `ruby -r`.
  No standalone `main()`, nothing to port.
- **astrogre** — has its own `are` grep CLI (separate from the embedder
  library); listed above.
