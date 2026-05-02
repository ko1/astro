# are — modern grep on the astrogre regex engine

`are` is a `grep`-style command-line tool on top of the
[`astrogre`](../) Ruby/Onigmo-compatible regex engine.  It plays in
the same space as [ripgrep (`rg`)](https://github.com/BurntSushi/ripgrep),
[the silver searcher (`ag`)](https://github.com/ggreer/the_silver_searcher),
and [`ack`](https://beyondgrep.com/) — keeping the muscle memory of
`grep` flags but switching the defaults to what people actually want
when searching a code tree.

## Quick start

```sh
$ make                          # build
$ ./are 'TODO'                  # search the cwd recursively, hidden+binary skipped
$ ./are -t rust 'fn main'       # restrict to Rust files
$ ./are -T md 'cargo'           # everything BUT Markdown
$ ./are -i -n -A 2 'panic!'     # case-ins, line numbers, 2 trailing context lines
$ ./are --type-list             # show the file-type table
```

## Why it exists

The astrogre engine library needs a CLI you'd actually keep on your
PATH: GNU grep's defaults (PATH required, no recursion without `-r`,
dotfiles included, no file-type filter) make sense for the original
1970s setting but get in the way day-to-day on a code base.  `are` is
that CLI — same engine, modern UX.

Engine self-test and microbench live as Makefile targets in the parent
directory (`make self-test`, `make bench`, `make bench-file`); they
don't need a CLI surface.  Three dev/debug flags
(`--dump PATTERN`, `--verbose`, `--cs-verbose`) are built into `are`
itself but intentionally absent from `--help` to keep the user-facing
manual focused.

## Defaults that differ from GNU grep

| Behaviour                              | grep        | are          |
| -------------------------------------- | ----------- | ------------ |
| PATH required                          | yes         | defaults `.` |
| Recurse into directories               | `-r`        | always       |
| Skip hidden (`.foo`) entries           | no          | yes          |
| Skip binary files                      | no          | yes          |
| Colour output on a tty                 | distro-dep. | yes          |
| Pattern syntax                         | BRE         | Ruby/Onigmo  |
| Default encoding                       | locale      | UTF-8        |

To get back grep-like behaviour: `--no-recursive`, `--hidden`, `-a`
(text mode), `--color=never`.

## Feature comparison

| Feature                      | grep | ag | ack | rg | **are** |
| ---------------------------- | :--: | :-: | :-: | :-: | :-: |
| Recursive default            |  -   | ✓  | ✓  | ✓  | ✓ |
| `.gitignore` aware           |  -   | ✓  | -  | ✓  | ✓ |
| Skip binary by default       |  -   | ✓  | ✓  | ✓  | ✓ |
| Skip hidden by default       |  -   | ✓  | ✓  | ✓  | ✓ |
| Multi-thread file walking    |  -   | ✓  | -  | ✓  | ✓ |
| Type filter (`-t LANG`)      |  -   | ✓  | ✓  | ✓  | ✓ |
| User-defined types           |  -   | ✓  | ✓  | ✓  | ✓ |
| Look-around / named captures | `-P` | -  | ✓  | `-P` | ✓ |
| Onigmo extensions (cond / recursion / atomic / `\g<name>`) | - | - | - | - | ✓ |
| Engine                       | DFA  | NFA + Onigmo MatchCache | NFA (PCRE) | hybrid lazy DFA + NFA fallback | NFA backtracking + Onigmo MatchCache + SIMD prefilter |
| ReDoS-safe by default ¹      | ✓ (BRE/ERE) · - under `-P` | ✓ | - | ✓ default · - under `-P` | partial ² |
| AOT specialisation cache     |  -   | -  | -  | -  | ✓ (`--aot`) |
| Embeddable as C library      |  -   | -  | -  | -  | ✓ (`astrogre.h`) |

¹ "ReDoS-safe" = pathological patterns (e.g. `(a+)+b`) cannot push a
  full scan into exponential time on adversarial input.  This is a
  property of the engine, not the CLI — `grep -P` and `rg -P` lose it
  because they switch to PCRE2's backtracking NFA.

² The `astrogre` engine carries an Onigmo-style MatchCache: lazily
  allocated once `backtrack_count > strlen × n_branches`, and gated
  by a static eligibility check at parse time.  Catches the usual
  catastrophic-backtracking shapes (`(a+)+b`, `(a|a)*b`, …) — those
  finish in linear time on adversarial input.  The cache is *off*
  for patterns containing backreferences, atomic groups (`(?>…)`),
  subroutine calls (`\g<name>`), conditional groups (`(?(cond)…)`),
  or captures inside look-around — these constructs make
  `(node_id, pos) → known-fail` memoization unsound (different code
  paths reaching the same `(id, pos)` can have different captures
  or recursion depth and so different outcomes), so we fall back to
  plain backtracking on them.  This residual hole is *not* closable
  by adding a lazy DFA: backreferences put the language outside the
  regular class entirely, and atomic / subroutine / conditional
  carry stateful semantics a DFA can't represent.  The realistic
  mitigation for the ineligible subset is a runaway-detector
  (abort after N steps with a clear error) rather than a different
  matching algorithm.  Until that's in, treat memo-ineligible
  patterns as untrusted-input-unsafe.

## Options

```
Match options:
  -i  case-insensitive            -n  print line numbers
  -c  count matching lines        -v  invert match
  -w  whole word                  -x  match whole line
  -F  fixed string                -q  quiet (exit code only)
  -l  files with matches          -L  files without
  -H  always show filename        -h  never show filename
  -o  only matching parts         -Z  NUL-separated filenames
  -m N  stop after N matches/file
  -A N / -B N / -C N              context lines (after / before / both)
  -e PATTERN (repeatable)         -f FILE  read patterns from FILE

Walking options:
  -t LANG / -T LANG               include / exclude file type
  --type-add NAME:GLOB[:GLOB...]  define a custom type
  --type-list                     list known types and exit
  --include=GLOB / --exclude=GLOB extra basename glob filters
  --hidden                        descend into hidden entries
  --no-ignore                     ignore .gitignore files
  -a, --text                      do NOT skip binary files
  --no-recursive                  do NOT descend into directories
  -j N                            parallel workers (default: NCPU)

Other:
  --color=never|always|auto       (default auto via isatty)
  --engine=astrogre|onigmo        backend selection (default astrogre)
  --aot                           AOT-specialise patterns to code_store/
  -V, --version                   print version
      --help                      print help
```

## Built-in file types

Run `are --type-list` for the full table.  The starter set covers ~50
languages and configuration formats: `c`, `cpp`, `rust`, `go`,
`ruby`, `python`/`py`, `js`, `ts`, `java`, `kotlin`, `swift`,
`haskell`, `ocaml`, `lua`, `perl`, `php`, `sh`, `sql`, `html`,
`css`, `md`, `tex`, `json`, `yaml`, `toml`, `xml`, `make`, `cmake`,
`docker`, `proto`, `asm`, `vim`, `r`, `dart`, `zig`, `nim`,
`crystal`, … (extend it in `types.c` or via `--type-add`).

## Architecture

Most of `are` is a thin shell over the astrogre engine:

```
are/
├── main.c    ← CLI entry, opt parser, recursive walker, mmap fast paths
├── types.c   ← `-t LANG` table + lookup
├── types.h
├── ignore.c  ← .gitignore parser + per-dir matcher stack
├── ignore.h
├── work.c    ← bounded MPMC queue + worker pool for `-j N`
├── work.h
├── Makefile  ← reuses parent astrogre/ sources directly
└── README.md
```

The walker (in `main.c`) is the producer: it descends directories
single-threaded — the `.gitignore` stack is push/pop'd at each
opendir/closedir, which only the producer touches — and submits one
file per scan to the work pool.  Workers pop, run `process_file` with
their own `grep_state` (cloned from the base; per-thread match
counter; per-task `open_memstream` output buffer), then flush the
buffer to real `stdout` under a single mutex.  This keeps per-file
output blocks coherent (no interleaved match lines) while letting the
scan itself run on N cores.

The Makefile pulls `node.c parse.c match.c selftest.c
backend_astrogre.c` straight from the parent `astrogre/` directory,
along with the codegen products (`node_eval.c` etc.) — there's no
separate static library, just one `cc` invocation per build.

`main.c` was originally forked from a now-removed legacy CLI; it's
the only grep front-end now.  Three dev flags
(`--dump` / `--verbose` / `--cs-verbose`) live here but stay out of
`--help`; engine self-test / microbench moved to Makefile targets
(`make self-test` / `make bench` / `make bench-file`).

## Performance

Two distinct workloads, two different stories.  All numbers are
ms / best-of-3, on a 118 MB C-source corpus or `/usr/include`
respectively.  Output goes to a regular file, never `/dev/null` —
GNU grep `fstat`s stdout and short-circuits to first-match-and-exit
when it sees `/dev/null`, which would skew the comparison wildly.
See [`../docs/perf.md`](../docs/perf.md) for the full discussion
and reproduction instructions.

### Single-file scan (`bench/grep_bench.rb`)

`are` shares the engine with `astrogre`, so single-file numbers
track the engine directly.  Numbers below are **ms / best-of-7**
on a 118 MB C-source corpus, single core (`taskset -c 4`), output
to a regular file.  Bold cell = row minimum (the fastest tool on
that pattern).  The `+AOT cached` column reports the warm-cache
time only; the cold path adds a one-shot `cc` compile of ≈ 400 ms
(steady-state thereafter).

| pattern                          | are -j1 | are +AOT cached | ripgrep | GNU grep |
|----------------------------------|--------:|----------------:|--------:|---------:|
| `/static/`                       | **38**  | 41              | 41      | 74       |
| `/specialized_dispatcher/`       | **21**  | 24              | **21**  | 38       |
| `/^static/` anchored             | 77      | 80              | **41**  | 76       |
| `/VALUE/i`                       | 85      | 76              | **56**  | 98       |
| `/static\|extern\|inline/`       | 376     | 121             | **63**  | 205      |
| 12-way alt (AC prefilter)        | 444     | 409             | **139** | 276      |
| `/[0-9]{4,}/`                    | 362     | 168             | **55**  | 97       |
| `/[a-z_]+_[a-z]+\(/`             | 1858    | 1202            | **203** | 2214     |
| `-c /static/`                    | **22**  | 24              | 27      | 64       |

Honest row count: **`are` 2 wins + 1 tie · `rg` 6 wins + 1 tie ·
`grep` 0 wins** on this set of 9 patterns.  For ad-hoc grep-style
usage on a single file, **`rg` is still the better default tool.**
The cases where `are` does win are all in the literal / count
family where the engine has a specialised SIMD path.

Where `are` wins outright:

- **`-c /static/`** (count): `are` 22 ms vs `rg` 27 ms vs `grep`
  64 ms.  The `node_grep_count_lines_lit` path is a single
  specialised dispatcher that does SIMD scan + line-skip + count
  in one tight loop.
- **`/static/` default print**: `are` 38 ms vs `rg` 41 ms vs
  `grep` 74 ms.  Same SIMD scanner plus an inlined emit-line
  action.
- **`/specialized_dispatcher/`**: tied with `rg` at 21 ms — long
  literal, tight `memmem`.

Where AOT helps (`are -j1` → `are +AOT cached`):

| pattern                 | interp  | AOT      | speed-up |
|-------------------------|--------:|---------:|---------:|
| `static\|extern\|inline`| 376 ms  | 121 ms   | **3.1×** |
| `[0-9]{4,}`             | 362 ms  | 168 ms   | **2.2×** |
| `[a-z_]+_[a-z]+\(`      | 1858 ms | 1202 ms  | **1.5×** |
| `VALUE/i`               | 85 ms   | 76 ms    | 1.1×     |

AOT has a small constant cost (≈ 2–4 ms for `dlopen` + dispatcher
swap) which shows up as a slight regression on already-fast
patterns (`-c /static/` 22 → 24 ms).  Worth turning on when (a)
the body chain has substantial work to fold (alt-of-LIT, greedy
class-rep) **and** (b) the same pattern is being re-used many
times — `--aot` caches per-pattern in `code_store/`, so ad-hoc
one-off greps with a fresh pattern each time pay the cold compile
each time and never reach the cached column.

Where ripgrep wins (and what would close the gap):

- **Anchored / `/i` / class-rep**: rg's literal-prefix prefilter
  plus lazy DFA beats AOT by 1.5–3×.
- **`/[a-z_]+_[a-z]+\(/`**: rg lazy DFA at 203 ms; we're at
  1.2 s even with AOT.  This is the biggest engine gap and it's
  structural — only a DFA-based path closes it.
- **Multi-literal alt (12-way)**: rg's Aho-Corasick + Teddy SIMD
  is 2–3× faster per byte than ours.  Pure engine work, tracked
  in [`../docs/todo.md`](../docs/todo.md).

### Tree walk (`bench/tree_bench.rb`)

Recursive search with `.gitignore`-aware filtering.  This is what
day-to-day code grep actually looks like.

| pattern              | tree           | are -j4 | ripgrep | grep -r |
|----------------------|----------------|--------:|--------:|--------:|
| `/CONFIG/`           | /usr/include   | 45      | **17**  | 76      |
| `PROT_READ\|PROT_W…` | /usr/include   | 41      | **18**  | 93      |
| 12-lit alt (AC)      | /usr/include   | 94      | **18**  | 144     |
| `/verbose_mark/`     | astro tree     | 106     | **31**  | 474     |

(ms / best-of-5, `-j 4` for `are`, `-t c` for all three.)

`are` consistently beats `grep -r` by 2–5× on tree walk thanks to
`.gitignore` filtering, parallel file scanning (`-j 4` here), and
the read-fast-path for small files.  But ripgrep wins by 2–5×
back, with two reasons stacked:

1. **Parallel directory walking.**  rg's `ignore` crate parallelises
   the walk itself; our walker is single-threaded producer + N
   worker scanners.  The producer becomes the bottleneck on big
   trees with many small files.
2. **Algorithmic prefilter (engine).**  Same single-file gap shows
   up here too: rg's Aho-Corasick + Teddy on multi-literal cases.

## Why use `are` over `rg` or `ag`?

Honest version: **for plain `-c` and default-print on a single
file you probably won't notice a difference vs ripgrep — `are`
already wins those rows.**  For tree walks on a large repo, `rg`
is still the king (typically 2–5× ahead) and that won't change
without the parallel-walker and Teddy work.

Where `are` is the right tool:

1. **Ruby/Onigmo regex semantics out of the box.**  `rg -P` enables
   PCRE2 (close but not identical to Onigmo); `are` is byte-for-byte
   the same `Regexp` engine semantics you get in Ruby.  Look-behind,
   named captures, conditional groups, subroutine calls, set
   operations on character classes, and Onigmo MatchCache for ReDoS
   mitigation on the eligible pattern subset — all on by default,
   no flag needed.  See the *ReDoS-safe by default* row in the
   feature table above for the cache-ineligible constructs.
2. **AOT pattern specialisation.**  `--aot` writes per-pattern
   specialised C to `code_store/` and links it back in.  For a
   pattern you grep with hundreds of times (CI scripts, editor
   integration), the per-invocation match loop becomes a tight,
   pattern-shape-aware basic block.  Nobody else in the grep family
   does this.
3. **Embeddable.**  `astrogre` exposes a stable C ABI and a Ruby C
   extension (`require "astrogre"`), so the same engine that powers
   `are` can be dropped into Logstash-style pipelines, editor
   tooling, or app-level validators without a separate compile.

What we have NOT done yet (and where `rg` will keep winning until we
do):

- **Multi-pattern Aho-Corasick / Teddy literal prefilter** for
  arbitrary positions in the pattern (we have AC for leading
  alts only).  Belongs in the astrogre engine, not in `are`.
- **Parallel directory walking.**  Single-threaded producer
  today; rg uses `rayon` to parallelise the dir walk too.

## Roadmap

In rough order of expected impact:

1. **Multi-pattern anchor in the astrogre engine** — extract
   necessary literals from the compiled IRE, build an
   Aho-Corasick / Teddy scanner, replace the single-literal
   `memmem` prefilter.  Lives in `astrogre/`, not here.
2. **`.gitignore` polish** — global `core.excludesFile`,
   `.git/info/exclude`, mid-path double-star.  v1 covers the 95%
   case; these are the long tail.
3. **Smart-case** (`-S`): if the pattern has no uppercase, behave as
   `-i`; else case-sensitive.  Trivial.
4. **`--files`** — list every file `are` would otherwise search.
   Useful for piping to other tools and for debugging the walker.
5. **JSON output** (`--json`) — for editor integration / piping into
   `jq`.  Mirrors `rg --json`.
6. **Adaptive `-j`** — drop worker count on small input where
   thread setup outweighs the parallelism win, raise on large.
7. **Parallel directory walking** — currently the producer is
   single-threaded.  On NVMe storage with cold cache this can be
   IO-bound; spreading dir reads across workers helps.

The astrogre engine roadmap is in [`../docs/todo.md`](../docs/todo.md).

## See also

- Parent engine and library docs: [`../README.md`](../README.md),
  [`../docs/done.md`](../docs/done.md), [`../docs/todo.md`](../docs/todo.md).
- Ripgrep design notes:
  https://blog.burntsushi.net/ripgrep/ — required reading if you
  want to understand where the throughput targets come from.
