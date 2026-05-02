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

`astrogre` has a perfectly serviceable grep front-end already
(`../astrogre`), but it inherited GNU grep's defaults: PATH required,
no recursion without `-r`, dotfiles included, no file-type filter.
That makes it fine for engine testing and for benchmarking against
GNU grep, but awkward to actually use day-to-day on a code base.

`are` is the production CLI — same engine, modern UX.  The two
binaries coexist; the legacy `astrogre` binary still owns the
library-internal sub-commands (`--self-test`, `--bench`, `--dump`,
`--via-prism`, `--verbose`, …) which `are` deliberately doesn't
expose.

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
| Look-around / named captures |  -   | -  | ✓  | `-P` | ✓ |
| Onigmo MatchCache (ReDoS)    |  -   | -  | -  | -  | ✓ |
| AOT specialisation cache     |  -   | -  | -  | -  | ✓ (`--aot`) |

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

`main.c` is currently a fork of `../main.c` with modernised defaults
and the lib-internal sub-commands stripped.  Consolidating it into a
shared `lib_grep.c` with two thin entry points (one per binary) is
on the roadmap.

## Performance

Two distinct workloads, two different stories.  All numbers are
ms / best-of-3, on a 118 MB C-source corpus or `/usr/include`
respectively.  Output goes to a regular file, never `/dev/null` —
GNU grep `fstat`s stdout and short-circuits to first-match-and-exit
when it sees `/dev/null`, which would skew the comparison wildly.
See [`../docs/perf.md`](../docs/perf.md) for the full discussion
and reproduction instructions.

### Single-file scan (`bench/grep_bench.rb`)

`are` is the production CLI on the same engine that powers
`astrogre`, so its single-file numbers track the engine's directly.
Notable rows (use `make bench-rg N=5` to reproduce):

| pattern                       | are -j1 | ripgrep | GNU grep |
|------------------------------ |--------:|--------:|---------:|
| `/static/`                    | **41**  | 86      | 153      |
| `/specialized_dispatcher/`    | 23      | **24**  | 36       |
| `/^static/` anchored          | 86      | **42**  | 78       |
| `/VALUE/i`                    | 94      | **64**  | 107      |
| `/static\|extern\|inline/`    | 402     | **69**  | 224      |
| 12-way alt (uses our AC)      | 428     | **142** | 281      |
| `/[0-9]{4,}/`                 | 404     | 60      | **96**   |
| `/[a-z_]+_[a-z]+\(/`          | 2154    | **230** | 2478     |
| `-c /static/`                 | **24**  | 30      | 77       |

Where `are` wins:

- `-c /static/` (count): 24 ms vs grep 77 ms vs rg 30 ms.  The
  `node_grep_count_lines_lit` path is a single specialised SD that
  does SIMD scan + line-skip + count in one tight loop.
- `/static/` default print: 41 ms vs grep 153 ms vs rg 86 ms.
  Same scanner + an inlined emit-line action.

Where `are` loses:

- Anchored / `/i` / class-rep: ripgrep's literal-prefix prefilter
  + lazy DFA wins by 1.5–2×.  Closing this needs DFA-style NFA
  simulation — a bigger refactor.
- `[a-z_]+_[a-z]+\(`: rg's lazy DFA gets to 230 ms, we're at
  2.2 s.  The DFA shape advantage is biggest where prefilter
  doesn't apply.
- Multi-literal alt: our AC kicks in for 9+ distinct first bytes,
  but rg's Aho-Corasick + Teddy SIMD lookup is 2–3× faster
  per byte.  Engine work tracked in
  [`../docs/todo.md`](../docs/todo.md).

### Tree walk (`bench/tree_bench.rb`)

Recursive search with `.gitignore`-aware filtering.  This is what
day-to-day code grep actually looks like.

| pattern              | tree           | are -j4 | ripgrep | grep -r |
|----------------------|----------------|--------:|--------:|--------:|
| `/CONFIG/`           | /usr/include   | 49      | **20**  | 88      |
| `PROT_READ\|PROT_W…` | /usr/include   | 45      | **22**  | 101     |
| 12-lit alt (AC)      | /usr/include   | 109     | **21**  | 164     |
| `/verbose_mark/`     | astro tree     | 134     | **37**  | 605     |

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
   the same engine you get from `Regexp` in Ruby.  Look-behind,
   named captures, conditional groups, subroutine calls, set
   operations on character classes, MatchCache for ReDoS — all on
   by default, no flag needed.
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
